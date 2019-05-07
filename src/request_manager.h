#ifndef _REQUESTMANAGER_H_
#define _REQUESTMANAGER_H_

#include "cham_statistics.h"

#include <mpi.h>
#include <vector>
#include <queue>
#include <unordered_map>
#include <atomic>
#include <functional>

typedef enum RequestType {
    send        = 0, 
    recv        = 1, 
    recvData    = 2, 
    sendBack    = 3, 
    recvBack    = 4
} RequestType;

static const char* RequestType_values[] = {
    "send",         // 0
    "recv",         // 1
    "recvData",     // 2
    "sendBack",     // 3
    "recvBack"      // 4
};

class RequestManager {
  public:
    RequestManager();
    void submitRequests( int tag, int rank, int n_requests, 
                         MPI_Request *requests,
                         bool block,
                         std::function<void(void*, int, int, cham_migratable_task_t*)> handler,
                         RequestType type,
                         void* buffer=NULL,
                         cham_migratable_task_t* task=NULL);
    void progressRequests();
    int getNumberOfOutstandingRequests();
    void printRequestInformation();

  private:
    struct RequestGroupData {
        void *buffer;
        std::function<void(void*, int, int, cham_migratable_task_t*)> handler;
        int rank;
        int tag;
        RequestType type;
        double start_time;
        cham_migratable_task_t* task;
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
    std::vector<MPI_Request> _current_request_array;
    std::unordered_map<int, int> _current_vecid_to_rid;
    std::atomic<int> _current_num_finished_requests;
    std::unordered_map<int, RequestGroupData> _map_id_to_request_group_data;
    std::unordered_map<int, RequestData> _map_rid_to_request_data;
    std::unordered_map<int, int> _outstanding_reqs_for_group;

    std::atomic<int> _num_posted_requests[5];
    std::atomic<int> _num_completed_requests[5];
    std::atomic<int> _num_posted_request_groups[5];
    std::atomic<int> _num_completed_request_groups[5];
};

#endif
