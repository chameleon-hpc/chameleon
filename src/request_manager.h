#ifndef _REQUESTMANAGER_H_
#define _REQUESTMANAGER_H_

#include "cham_statistics.h"

#include <mpi.h>
#include <vector>
#include <queue>
#include <unordered_map>
#include <atomic>
#include <functional>

enum RequestType {send, recv, recvData, sendBack, recvBack};

class RequestManager {
  public:
    RequestManager();
    void submitRequests( int tag, int rank, int n_requests, 
                         MPI_Request *requests,
                         bool block,
                         std::function<void(void*, int, int)> handler,
                         RequestType type,
                         void* buffer=NULL);
    void progressRequests();
    int getNumberOfOutstandingRequests();

  private:
    struct RequestGroupData {
        void *buffer;
        std::function<void(void*, int, int)> handler;
        int rank;
        int tag;
        RequestType type;
        double start_time;
    };

    struct RequestData {
        int gid;
        MPI_Request mpi_request;
    };

#if CHAM_STATS_RECORD
    void addTimingToStatistics(double elapsed, RequestType type);
#endif
    std::atomic<int> _id;
    std::atomic<int> _groupId;
    std::queue<int> _request_queue;
    std::unordered_map<int, RequestGroupData> _map_id_to_request_group_data;
    std::unordered_map<int, RequestData> _map_rid_to_request_data;
    std::unordered_map<int, int> _outstanding_reqs_for_group;
};

#endif
