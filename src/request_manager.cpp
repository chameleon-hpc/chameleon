#include "request_manager.h"

#define MAX_REQUESTS 100

RequestManager::RequestManager()
 : _id(0), _groupId(0) {

}

void RequestManager::submitRequests( int tag, int rank, int n_requests, 
                                MPI_Request *requests,
                                bool block, 
                                std::function<void(void*, int, int)> handler,
                                void* buffer) {
  if(block) {
     MPI_Waitall(n_requests, &requests[0], MPI_STATUSES_IGNORE);
     handler(buffer, tag, rank);
     //for(int i=0; i<buffers_to_delete.size(); i++) delete[] buffers_to_delete[i];
     return;
  }
 
  int gid=_groupId++;
  RequestGroupData request_group_data = {buffer, handler, rank, tag};
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
  std::vector<MPI_Request> requests;
  std::unordered_map<int, int> vecid_to_rid;

  for(int i=0; i<MAX_REQUESTS && !_request_queue.empty(); i++) {
    int rid = _request_queue.front();
    _request_queue.pop();
    MPI_Request request = _map_rid_to_request_data[rid].mpi_request;
    requests.push_back(request);  
    vecid_to_rid.insert(std::make_pair(i, rid));
  }

  int n_requests = requests.size();
 
  if(n_requests==0) return;  

  int outcount = 0;
  int *arr_of_indices = new int[n_requests];
  MPI_Status *arr_of_statuses = new MPI_Status[n_requests];  

  MPI_Testsome(n_requests, &requests[0], &outcount, arr_of_indices, arr_of_statuses );

  for(int i=0; i<outcount; i++) {
    int idx = arr_of_indices[i];
    int rid = vecid_to_rid[idx];

    RequestData request_data = _map_rid_to_request_data[rid];
    int gid = request_data.gid;
    _outstanding_reqs_for_group[gid]--;
    _map_rid_to_request_data.erase(rid);
   
    if(_outstanding_reqs_for_group[gid]==0) { 
       _outstanding_reqs_for_group.erase(gid);
       RequestGroupData request_group_data = _map_id_to_request_group_data[gid];

       std::function<void(void*, int, int)> handler = request_group_data.handler;
       void* buffer = request_group_data.buffer;
       int tag = request_group_data.tag;
       int rank = request_group_data.rank;
               
       handler(buffer, tag, rank);

       _map_id_to_request_group_data.erase(gid);
    } 
  }

  for(int i=0; i<n_requests; i++) {
    if(requests[i]!=MPI_REQUEST_NULL) {
      _request_queue.push(vecid_to_rid[i]);
    }
  }

}
