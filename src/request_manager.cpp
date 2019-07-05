#include "request_manager.h"

#include <omp.h>
#include <cassert>
#include "chameleon_common.h"

#define MAX_REQUESTS 10000000

RequestManager::RequestManager()
 : _id(0), _groupId(0), _current_request_array(0), _current_num_finished_requests(0) {
 
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
  
    _num_posted_requests[type]+= n_requests; 
    _num_posted_request_groups[type]+=1;

    int canFinish = 0;
    int ierr = MPI_Testall(n_requests, &requests[0], &canFinish, MPI_STATUSES_IGNORE);
    assert(ierr==MPI_SUCCESS);
    
    if(canFinish) {
#if CHAM_STATS_RECORD   
    double elapsed = omp_get_wtime()-startStamp;
    if(type == send || type == sendBack) {
        add_throughput_send(elapsed, sum_bytes);
    } else {
    #if OFFLOAD_DATA_PACKING_TYPE > 0 // disable tracking for very short meta data messages because it might destroy reliability of statistics
        if(type != recv)
    #endif
            add_throughput_recv(elapsed, sum_bytes);
    }
#endif 
      _num_completed_requests[type]+= n_requests;
      _num_completed_request_groups[type]+=1;
      
      handler(buffer, tag, rank, tasks, num_tasks);
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
  _outstanding_reqs_for_group.insert(std::make_pair(gid, n_requests));

  for(int i=0; i<n_requests; i++) {
    int rid=_id++;
 
    RequestData request_data = {gid, requests[i]};

    _map_rid_to_request_data.insert(std::make_pair(rid, request_data));
    _request_queue.push(rid);
  }
}

void RequestManager::progressRequests() {
  //std::vector<MPI_Request> requests;
  //std::unordered_map<int, int> vecid_to_rid;

    /*int i = 0;
    while(!_request_queue.empty()) {
        int rid = _request_queue.front();
        _request_queue.pop();
        MPI_Request request = _map_rid_to_request_data[rid].mpi_request;
        _current_request_array.push_back(request);
        vecid_to_rid.insert(std::make_pair(i, rid));
        i++;
    }*/
 
  if(_current_request_array.size()==0) {
    for(int i=0; i<MAX_REQUESTS && !_request_queue.empty(); i++) {
      int rid = _request_queue.front();
      _request_queue.pop();
      MPI_Request request = _map_rid_to_request_data[rid].mpi_request;
      _current_request_array.push_back(request);  
      _current_vecid_to_rid.insert(std::make_pair(i, rid));
    }
  }

  int n_requests = _current_request_array.size();
 
  if(n_requests==0) return;  

  int outcount = 0;
  std::vector<int> arr_of_indices(n_requests);
  std::vector<MPI_Status> arr_of_statuses(n_requests);

#if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
   double cur_time = omp_get_wtime();
#endif
  MPI_Testsome(n_requests, &_current_request_array[0], &outcount, &(arr_of_indices[0]), &(arr_of_statuses[0]) );
#if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
   cur_time = omp_get_wtime()-cur_time;
   if(cur_time>CHAM_SLOW_COMMUNICATION_THRESHOLD)
     _num_slow_communication_operations++; 
#endif
  _current_num_finished_requests += outcount;

  for(int i=0; i<outcount; i++) {
    int idx = arr_of_indices[i];
    int rid = _current_vecid_to_rid[idx];

    RequestData request_data = _map_rid_to_request_data[rid];
    int gid = request_data.gid;
    
    RequestGroupData rg_data = _map_id_to_request_group_data[gid];
    _num_completed_requests[rg_data.type]++;

     _outstanding_reqs_for_group[gid]--;
    _map_rid_to_request_data.erase(rid);
   
    if(_outstanding_reqs_for_group[gid]==0) {
#if CHAM_STATS_RECORD 
       double finishedStamp = omp_get_wtime();
#endif
       _outstanding_reqs_for_group.erase(gid);
       RequestGroupData request_group_data = _map_id_to_request_group_data[gid];
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
#endif
       handler(buffer, tag, rank, tasks, num_tasks);
       _map_id_to_request_group_data.erase(gid);
    }
  }

  if(_current_num_finished_requests==n_requests) {
    _current_request_array.clear();
    _current_vecid_to_rid.clear();
    _current_num_finished_requests=0; 
  }
  //for(int i=0; i<n_requests; i++) {
  //  if(requests[i]!=MPI_REQUEST_NULL) {
  //    _request_queue.push(vecid_to_rid[i]);
  //  }
  //}
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
  return _request_queue.size()-_current_num_finished_requests;
}
