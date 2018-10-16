#ifndef _REQUESTMANAGER_H_
#define _REQUESTMANAGER_H_

#include <mpi.h>
#include <vector>
#include <queue>
#include <unordered_map>
#include <atomic>
#include <functional>

class RequestManager {
  public:
    RequestManager();
    void submitRequests( int tag, int rank, int n_requests, 
                         MPI_Request *requests,
                         bool block,
                         std::function<void(void*, int, int)> handler,
                         void* buffer=NULL);
    void progressRequests();

  private:
    std::atomic<int> _id;
    std::atomic<int> _groupId;
    std::queue<int> _request_queue;
    std::unordered_map<int, void*> _map_id_to_buffers;
    std::unordered_map<int, std::function<void(void*, int, int)>> _map_id_to_handler;
    std::unordered_map<int, int> _map_id_to_rank;
    std::unordered_map<int, int> _map_id_to_tag;
    std::unordered_map<int, int> _map_rid_to_gid;
    std::unordered_map<int, MPI_Request> _map_rid_to_request;
    std::unordered_map<int, int> _outstanding_reqs_for_group;
};

#endif
