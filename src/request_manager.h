#ifndef _REQUESTMANAGER_H_
#define _REQUESTMANAGER_H_

#include "chameleon_statistics.h"

#include <mpi.h>
#include <vector>
// #include <queue>
#include <deque>
#include <unordered_map>
#include <atomic>
#include <functional>

typedef enum RequestType {
    send        = 0, 
    recv        = 1, 
    recvData    = 2, 
    sendBack    = 3, 
    recvBack    = 4,
    recvBackTrash = 5,
    cancel       = 6
} RequestType;

static const char* RequestType_values[] = {
    "send",         // 0
    "recv",         // 1
    "recvData",     // 2
    "sendBack",     // 3
    "recvBack",      // 4
    "recvBackTrash", // 5
    "cancel"
};

class RequestManager {
  public:

    int _num_threads_in_dtw;
    RequestManager();
    void submitRequests( double startStamp, int tag, int rank, int n_requests, 
                         MPI_Request *requests,
                         int sum_bytes,
                         bool block,
                         std::function<void(void*, int, int, cham_migratable_task_t**, int)> handler,
                         RequestType type,
                         void* buffer=NULL,
                         cham_migratable_task_t** tasks=NULL,
                         int num_tasks=0);
    void progressRequests();
    int getNumberOfOutstandingRequests();
    void printRequestInformation();

  private:
    struct RequestGroupData {
        void *buffer;
        std::function<void(void*, int, int, cham_migratable_task_t**, int)> handler;
        int rank;
        int tag;
        RequestType type;
        double start_time;
        int sum_bytes;
        cham_migratable_task_t **tasks;
        int num_tasks;
    };

    struct RequestData {
        int gid;
        MPI_Request mpi_request;
    };

    struct ThreadLocalRequestInfo {
        std::vector<MPI_Request>        current_request_array;
        std::unordered_map<int, int>    current_vecid_to_rid;
        std::atomic<int>                current_num_finished_requests;

        ThreadLocalRequestInfo() : current_request_array(0), current_num_finished_requests(0) {

        }
    };

    std::atomic<int> _id;
    std::atomic<int> _groupId;
    thread_safe_deque_t<int> _request_queue;
    std::vector<ThreadLocalRequestInfo*> _thread_request_info; // currently fixed 200 to avoid locking and stuff

    thread_safe_unordered_map<int, RequestGroupData>    _map_id_to_request_group_data;
    thread_safe_unordered_map<int, RequestData>         _map_rid_to_request_data;
    thread_safe_unordered_map_atomic<int, int>          _outstanding_reqs_for_group;

    std::atomic<int> _num_posted_requests[5];
    std::atomic<int> _num_completed_requests[5];
    std::atomic<int> _num_posted_request_groups[5];
    std::atomic<int> _num_completed_request_groups[5];

    ThreadLocalRequestInfo* get_request_info_for_thread();
};

#endif
