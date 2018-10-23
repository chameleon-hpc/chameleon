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
    struct RequestGroupData {
        void *buffer;
        std::function<void(void*, int, int)> handler;
        int rank;
        int tag;
    };

    struct RequestData {
        int gid;
        MPI_Request mpi_request;
    };

    std::atomic<int> _id;
    std::atomic<int> _groupId;
    std::queue<int> _request_queue;
    std::unordered_map<int, RequestGroupData> _map_id_to_request_group_data;
    std::unordered_map<int, RequestData> _map_rid_to_request_data;
    std::unordered_map<int, int> _outstanding_reqs_for_group;
};

#endif
