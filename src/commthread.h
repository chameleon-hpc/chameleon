// Communication thread header
#ifndef _COMMTHREAD_H_
#define _COMMTHREAD_H_

#include <mpi.h>

#include "chameleon.h"
#include "request_manager.h"

// Special version with 2 ranks where master (rank 0) is always offloading to rank 1
#ifndef FORCE_OFFLOAD_MASTER_WORKER
#define FORCE_OFFLOAD_MASTER_WORKER 0
#endif

// Flag wether offloading in general is enabled or disabled
#ifndef OFFLOAD_ENABLED
#define OFFLOAD_ENABLED 1
#endif

// Allow offload as soon as sum of outstanding jobs has changed
#ifndef OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED
#define OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED 1
#endif

// Just allow a single offload and block offload until a local or remote task has been executed and the local load has changed again
#ifndef OFFLOAD_BLOCKING
#define OFFLOAD_BLOCKING 0
#endif

// determines how data (arguments) is packed and send during offloading
#ifndef OFFLOAD_DATA_PACKING_TYPE
#define OFFLOAD_DATA_PACKING_TYPE 0     // 0 = pack meta data and arguments to gether and send it with a single message (requires copy to buffer)
// #define OFFLOAD_DATA_PACKING_TYPE 1     // 1 = zero copy approach, only pack meta data (num_args, arg types ...) + separat send for each mapped argument
#endif

// Create a separate thread for offloads that expect mapped data to be transfered back
#ifndef OFFLOAD_CREATE_SEPARATE_THREAD
#define OFFLOAD_CREATE_SEPARATE_THREAD 0
#endif

//Specify whether blocking or non-blocking MPI should be used (blocking in the sense of MPI_Isend or MPI_Irecv followed by an MPI_Waitall)
#ifndef MPI_BLOCKING
#define MPI_BLOCKING 0
#endif

#ifndef OFFLOADING_STRATEGY_AGGRESSIVE
#define OFFLOADING_STRATEGY_AGGRESSIVE 0
#endif

// communicator for remote task requests
extern MPI_Comm chameleon_comm;
// communicator for sending back mapped values
extern MPI_Comm chameleon_comm_mapped;
// communicator for load information
extern MPI_Comm chameleon_comm_load;

extern int chameleon_comm_rank;
extern int chameleon_comm_size;

extern RequestManager request_manager_send;
extern RequestManager request_manager_receive;

extern std::vector<intptr_t> _image_base_addresses;

// list with data that has been mapped in map clauses
extern std::mutex _mtx_data_entry;
extern std::list<OffloadingDataEntryTy*> _data_entries;

// list with local task entries
// these can either be executed here or offloaded to a different rank
extern std::mutex _mtx_local_tasks;
extern std::list<TargetTaskEntryTy*> _local_tasks;
extern int32_t _num_local_tasks_outstanding;

// list with stolen task entries that should be executed
extern std::mutex _mtx_stolen_remote_tasks;
extern std::list<TargetTaskEntryTy*> _stolen_remote_tasks;
extern int32_t _num_stolen_tasks_outstanding;

// list with stolen task entries that need output data transfer
extern std::mutex _mtx_stolen_remote_tasks_send_back;
extern std::list<TargetTaskEntryTy*> _stolen_remote_tasks_send_back;

// entries that should be offloaded to specific ranks
extern std::mutex _mtx_offload_entries;
extern std::list<OffloadEntryTy*> _offload_entries;

// ====== Info about outstanding jobs (local & stolen) ======
// extern std::mutex _mtx_outstanding_jobs;
extern std::vector<int32_t> _outstanding_jobs_ranks;
extern int32_t _outstanding_jobs_local;
extern int32_t _outstanding_jobs_sum;
// ====== Info about real load that is open or is beeing processed ======
// extern std::mutex _mtx_load_info;
extern std::vector<int32_t> _load_info_ranks;
extern int32_t _load_info_local;
extern int32_t _load_info_sum;
// for now use a single mutex for box info
extern std::mutex _mtx_load_exchange;

#if OFFLOAD_BLOCKING
// only allow offloading when a task has finished on local rank
extern std::mutex _mtx_offload_blocked;
extern int32_t _offload_blocked;
#endif

// Threading section
extern int _comm_thread_load_exchange_happend;

#ifdef __cplusplus
extern "C" {
#endif

void print_arg_info(std::string prefix, TargetTaskEntryTy *task, int idx);

void print_arg_info_w_tgt(std::string prefix, TargetTaskEntryTy *task, int idx);

int32_t offload_task_to_rank(OffloadEntryTy *entry);

void* receive_remote_tasks(void *arg);

void* service_thread_action(void *arg);

int32_t start_communication_threads();

int32_t stop_communication_threads();

void trigger_update_outstanding();

#ifdef __cplusplus
}
#endif
#endif
