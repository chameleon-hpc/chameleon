#include "request_manager.h"

#include <omp.h>
#include <cassert>
#include "chameleon_common.h"
#include "commthread.h"

#define MAX_REQUESTS 10000000

RequestManager::RequestManager()
 : _id(0), _groupId(0), _thread_request_info(200*8) {
 
  _num_threads_in_dtw = -1;
  std::fill(&_num_posted_requests[0], &_num_posted_requests[5], 0);
  std::fill(&_num_completed_requests[0], &_num_completed_requests[5], 0);
  std::fill(&_num_posted_request_groups[0], &_num_posted_request_groups[5], 0);
  std::fill(&_num_completed_request_groups[0], &_num_completed_request_groups[5], 0);
}

void RequestManager::submitRequests( double startStamp, int tag, int rank, 
                                int n_requests, 
                                MPI_Request *requests,
                                int sum_bytes,
                                bool block,
                                std::function<void(void*, int, int, cham_migratable_task_t**, int)> handler,
                                RequestType type,
                                void* buffer,
                                cham_migratable_task_t** tasks,
                                int num_tasks) {
    #if CHAM_DEBUG
    std::string str_task_ids = "";
    if(tasks != NULL) {
        str_task_ids = std::to_string(tasks[0]->task_id);
        for(int i_tasks = 1; i_tasks < num_tasks; i_tasks++) {
            str_task_ids.append("," + std::to_string(tasks[i_tasks]->task_id));
        }
    }
    DBP("%s - submitting requests for tag %d and tasks %s\n", RequestType_values[type], tag, str_task_ids.c_str());


    #endif

#if CHAM_STATS_RECORD
    // time measurement for phase with communication (regardless whether it is send or recv)
    int tmp_active_comms = _num_active_communications_overall++;
    // DBP("_num_active_communications_overall is now\t%d\n", tmp_active_comms+1);
    if(tmp_active_comms == 0) {
        atomic_add_dbl(_time_communication_ongoing_sum, -1.0 * startStamp);
    }
#endif /* CHAM_STATS_RECORD */

    _num_posted_requests[type]+= n_requests; 
    _num_posted_request_groups[type]+=1;

    int canFinish = 0;
    MPI_Status *stats = new MPI_Status[n_requests];
    int ierr = MPI_Testall(n_requests, &requests[0], &canFinish, stats);
    if(ierr!=MPI_SUCCESS) {
       for(int i=0; i<n_requests;i++) {
          int eclass, len;
          char estring[MPI_MAX_ERROR_STRING];
          MPI_Error_class(stats[i].MPI_ERROR, &eclass);
          MPI_Error_string(stats[i].MPI_ERROR, estring, &len);
          fprintf(stderr, "Error %d: %s, requests: %d, type: %d\n", eclass, estring, n_requests, type);fflush(stderr);
       }
    }
    delete[] stats;
    assert(ierr==MPI_SUCCESS);
    
    if(canFinish) {
#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
    double elapsed = cur_time-startStamp;
    if(type == send || type == sendBack) {
        add_throughput_send(elapsed, sum_bytes);
    } else {
    #if OFFLOAD_DATA_PACKING_TYPE > 0 // disable tracking for very short meta data messages because it might destroy reliability of statistics
        if(type != recv)
    #endif
            add_throughput_recv(elapsed, sum_bytes);
    }
    int tmp_active_comms = --_num_active_communications_overall;
    // DBP("_num_active_communications_overall is now\t%d\n", tmp_active_comms);
    if(tmp_active_comms == 0) {
        atomic_add_dbl(_time_communication_ongoing_sum, cur_time);
    }
#endif 
      _num_completed_requests[type]+= n_requests;
      _num_completed_request_groups[type]+=1;
     
     try{ 
      handler(buffer, tag, rank, tasks, num_tasks);
     }
     catch(std::bad_function_call& e) {
      assert(false);
     }
      return;
    }

//   if(block) {
// #if CHAM_STATS_RECORD
//      double time = -omp_get_wtime();
// #endif
//      MPI_Status sta[n_requests];
//      int ierr= MPI_Waitall(n_requests, &requests[0], sta);
//      _num_completed_requests[type]+= n_requests;
//      _num_completed_request_groups[type]+=1; 
//      if(ierr!=MPI_SUCCESS) {
//         printf("MPI error: %d\n", ierr);
//         for(int i = 0; i < n_requests; i++) {
//             char err_msg[MPI_MAX_ERROR_STRING];
//             int err_len;
//             MPI_Error_string(sta[i].MPI_ERROR, err_msg, &err_len);
//             fprintf(stderr, "Arg[%d] Err[%d]: %s\n", i, sta[i].MPI_ERROR, err_msg);
//         }
//      }
//      assert(ierr==MPI_SUCCESS);
// #if CHAM_STATS_RECORD
//      time += omp_get_wtime();
//      // TODO: add throughput
// #endif
//      handler(buffer, tag, rank, tasks, num_tasks);
//      //for(int i=0; i<buffers_to_delete.size(); i++) delete[] buffers_to_delete[i];
//      return;
//   }
 
  int gid=_groupId++;
  RequestGroupData request_group_data = {buffer, handler, rank, tag, type, startStamp, sum_bytes, tasks, num_tasks};
  _map_id_to_request_group_data.insert(std::make_pair(gid, request_group_data));
  _outstanding_reqs_for_group.insert(gid, n_requests);

  for(int i=0; i<n_requests; i++) {
    int rid=_id++;
 
    RequestData request_data = {gid, requests[i]};

    DBP("Submitting request %d with type %s\n", rid, RequestType_values[type]);
    _map_rid_to_request_data.insert(std::make_pair(rid, request_data));
    _request_queue.push_back(rid);    
  }
}

void RequestManager::progressRequests() {
  ThreadLocalRequestInfo* req_info = get_request_info_for_thread();
  
  // calc number of requests to get from queue
  #if COMMUNICATION_MODE == 0
  int num_req_grab = MAX_REQUESTS;
  #elif COMMUNICATION_MODE == 1 || COMMUNICATION_MODE == 2
  int num_req_grab = (_request_queue.size() / _num_threads_in_dtw) + 1;
  #else // COMMUNICATION_MODE == 3 or 4
  int num_req_grab = (_request_queue.size() / (_num_threads_in_dtw+1)) + 1;
  #endif
  // try to grab min 2 requests for making progress
  if (num_req_grab < 2) num_req_grab = 2;

  if(req_info->current_request_array.size()==0) {
    int count_req_idx = 0;
    for(int i=0; i<num_req_grab && !_request_queue.empty(); i++) {
      bool succ = true;
      int rid = _request_queue.pop_front_b(&succ);
      if(succ) {
        MPI_Request request = _map_rid_to_request_data.get(rid).mpi_request;
        // get request type
        // int cur_gid = _map_rid_to_request_data.get(rid).gid;
        // RequestType cur_type = _map_id_to_request_group_data.get(cur_gid).type;
        // DBP("Grabbing request %d with type %s\n", rid, RequestType_values[cur_type]);
        req_info->current_request_array.push_back(request);  
        req_info->current_vecid_to_rid.insert(std::make_pair(count_req_idx, rid));
        count_req_idx++;
      }
    }
  }

  int n_requests = req_info->current_request_array.size(); 
  if(n_requests==0) return;

  int outcount = 0;
  std::vector<int> arr_of_indices(n_requests);
  std::vector<MPI_Status> arr_of_statuses(n_requests);

  // RELP("Checking %d requests in total\n", n_requests);

#if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
   double cur_time = omp_get_wtime();
#endif
  int ierr = MPI_Testsome(n_requests, &(req_info->current_request_array[0]), &outcount, &(arr_of_indices[0]), &(arr_of_statuses[0]) );
  if(ierr!=MPI_SUCCESS) {
     int eclass, len;
     char estring[MPI_MAX_ERROR_STRING];
     MPI_Error_class(ierr, &eclass);
     MPI_Error_string(ierr, estring, &len);
     fprintf(stderr, "Error %d: %s, requests: %d\n", eclass, estring, n_requests);fflush(stderr);
     for(int i=0; i<n_requests;i++) {
        fprintf(stderr, "Request at position %d: %ld\n", i, req_info->current_request_array[i]);
        MPI_Error_class(arr_of_statuses[i].MPI_ERROR, &eclass);
        MPI_Error_string(arr_of_statuses[i].MPI_ERROR, estring, &len);
        fprintf(stderr, "Error %d: %s, requests: %d\n", eclass, estring, n_requests);fflush(stderr);
     }
  }
  assert(ierr==MPI_SUCCESS);
#if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
   cur_time = omp_get_wtime()-cur_time;
   if(cur_time>CHAM_SLOW_COMMUNICATION_THRESHOLD)
     _num_slow_communication_operations++; 
#endif
  if(outcount >= 0) {
    DBP("Finished %d requests in total out of %d\n", outcount, n_requests);
    req_info->current_num_finished_requests += outcount;
  }

  for(int i=0; i<outcount; i++) {
    int idx = arr_of_indices[i];
    int rid = req_info->current_vecid_to_rid[idx];

    RequestData request_data = _map_rid_to_request_data.get(rid);
    int gid = request_data.gid;
    
    RequestGroupData request_group_data = _map_id_to_request_group_data.get(gid);
    _num_completed_requests[request_group_data.type]++;

    DBP("Finished request %d with type %s and tag %ld\n", rid, RequestType_values[request_group_data.type], request_group_data.tag);

    int remain_outstanding = _outstanding_reqs_for_group.decrement(gid); // threadsafe decrement and get
    _map_rid_to_request_data.erase(rid);
   
    if(remain_outstanding <= 0) {
#if CHAM_STATS_RECORD 
       double finishedStamp = omp_get_wtime();
#endif
       std::function<void(void*, int, int, cham_migratable_task_t**, int)> handler = request_group_data.handler;
       void* buffer = request_group_data.buffer;
       cham_migratable_task_t** tasks = request_group_data.tasks;
       int num_tasks = request_group_data.num_tasks;
       int tag = request_group_data.tag;
       int rank = request_group_data.rank;
       RequestType type = request_group_data.type;
       _num_completed_request_groups[type]++;
       DBP("%s - finally finished all requests for tag %ld\n", RequestType_values[type], tag);
#if CHAM_STATS_RECORD
       double startStamp = request_group_data.start_time;
       if(type == send || type == sendBack) {
            add_throughput_send(finishedStamp-startStamp, request_group_data.sum_bytes);
       } else {
            #if OFFLOAD_DATA_PACKING_TYPE > 0 // disable tracking for very short meta data messages because it might destroy reliability of statistics
            if(type != recv)
            #endif
                add_throughput_recv(finishedStamp-startStamp, request_group_data.sum_bytes);
       }
       int tmp_active_comms = --_num_active_communications_overall;
       // DBP("_num_active_communications_overall is now\t%d\n", tmp_active_comms);
       if(tmp_active_comms == 0) {
            atomic_add_dbl(_time_communication_ongoing_sum, finishedStamp);
       }
#endif
      try{
       handler(buffer, tag, rank, tasks, num_tasks);
      }
      catch (std::bad_function_call& e) {
       assert(false);
      }
       _map_id_to_request_group_data.erase(gid);
       _outstanding_reqs_for_group.erase(gid);
    }
  }

  if(req_info->current_num_finished_requests==n_requests) {
    req_info->current_request_array.clear();
    req_info->current_vecid_to_rid.clear();
    req_info->current_num_finished_requests=0;
  }
}

void RequestManager::printRequestInformation() {

 for(int i=0;i<5;i++) {
  fprintf(stderr, "Stats R#%d:\t_num_posted_requests[%s]\t%d\n", chameleon_comm_rank, RequestType_values[i], _num_posted_requests[i].load());
  fprintf(stderr, "Stats R#%d:\t_num_completed_requests[%s]\t%d\n", chameleon_comm_rank, RequestType_values[i], _num_completed_requests[i].load());
  fprintf(stderr, "Stats R#%d:\t_num_posted_request_groups[%s]\t%d\n", chameleon_comm_rank, RequestType_values[i], _num_posted_request_groups[i].load());
  fprintf(stderr, "Stats R#%d:\t_num_completed_request_groups[%s]\t%d\n", chameleon_comm_rank, RequestType_values[i], _num_completed_request_groups[i].load());
 }
 fprintf(stderr, "Stats R#%d:\t_num_outstanding_requests\t%d\n", chameleon_comm_rank, getNumberOfOutstandingRequests());
}

int RequestManager::getNumberOfOutstandingRequests() {
  ThreadLocalRequestInfo* req_info = get_request_info_for_thread();
  return _request_queue.size()-req_info->current_num_finished_requests;
}

inline RequestManager::ThreadLocalRequestInfo* RequestManager::get_request_info_for_thread() {
  RequestManager::ThreadLocalRequestInfo* val = _thread_request_info[__ch_get_gtid()*8]; // padding to avoid false sharing
  if(!val) {
    val = new RequestManager::ThreadLocalRequestInfo();
    _thread_request_info[__ch_get_gtid()*8] = val;
  }
  return val;
}
