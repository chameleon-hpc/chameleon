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
  _map_id_to_buffers.insert(std::make_pair(gid, buffer));
  _map_id_to_handler.insert(std::make_pair(gid, handler));
  _map_id_to_rank.insert(std::make_pair(gid, rank));
  _map_id_to_tag.insert(std::make_pair(gid, tag));
  _outstanding_reqs_for_group.insert(std::make_pair(gid, n_requests));

  for(int i=0; i<n_requests; i++) {
    int rid=_id++;
    _map_rid_to_gid.insert(std::make_pair(rid, gid));
    _map_rid_to_request.insert(std::make_pair(rid, requests[i]));
    _request_queue.push(rid);
  }
}

void RequestManager::progressRequests() {
  std::vector<MPI_Request> requests;
  std::unordered_map<int, int> vecid_to_rid;

  for(int i=0; i<MAX_REQUESTS && !_request_queue.empty(); i++) {
    int rid = _request_queue.front();
    _request_queue.pop();
    MPI_Request request = _map_rid_to_request[rid];
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
    int gid = _map_rid_to_gid[rid];
    _outstanding_reqs_for_group[gid]--;
    _map_rid_to_gid.erase(rid);
    _map_rid_to_request.erase(rid);
   
    if(_outstanding_reqs_for_group[gid]==0) { 
       _outstanding_reqs_for_group.erase(gid);
       std::function<void(void*, int, int)> handler = _map_id_to_handler[gid];
       void* buffer = _map_id_to_buffers[gid];
       int tag = _map_id_to_tag[gid];
       int rank = _map_id_to_rank[gid];
       
       handler(buffer, tag, rank);

       //for(int i=0; i<buf_to_delete.size(); i++) delete[] buf_to_delete[i]; 
       _map_id_to_rank.erase(gid);
       _map_id_to_tag.erase(gid);
       _map_id_to_handler.erase(gid);
	   _map_id_to_buffers.erase(gid);
    } 
  }

  for(int i=0; i<n_requests; i++) {
    if(requests[i]!=MPI_REQUEST_NULL) {
      _request_queue.push(vecid_to_rid[i]);
    }
  }

}
