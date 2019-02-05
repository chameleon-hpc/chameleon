// Communication thread header
#ifndef _COMMTHREAD_H_
#define _COMMTHREAD_H_

#include <mpi.h>

#include "chameleon.h"
#include "chameleon_common.h"
#include "request_manager.h"

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
extern thread_safe_task_list _local_tasks;
extern std::atomic<int32_t> _num_local_tasks_outstanding;

// list with stolen task entries that should be executed
extern thread_safe_task_list _stolen_remote_tasks;
extern std::atomic<int32_t> _num_stolen_tasks_outstanding;

// list with stolen task entries that need output data transfer
extern thread_safe_task_list _stolen_remote_tasks_send_back;

// for now use a single mutex for box info
extern std::mutex _mtx_load_exchange;
// ====== Info about outstanding jobs (local & stolen & offloaded (communication)) ======
extern std::vector<int32_t> _outstanding_jobs_ranks;
extern std::atomic<int32_t> _outstanding_jobs_local;
extern std::atomic<int32_t> _outstanding_jobs_sum;
// ====== Info about real load that is open or is beeing processed ======
extern std::vector<int32_t> _load_info_ranks;
extern int32_t _load_info_local;
extern int32_t _load_info_sum;

#if OFFLOAD_BLOCKING
// only allow offloading when a task has finished on local rank
extern std::atomic<int32_t> _offload_blocked;
#endif

// list that holds task ids (created at the current rank) that are not finsihed yet
extern std::mutex _mtx_unfinished_locally_created_tasks;
extern std::list<int32_t> _unfinished_locally_created_tasks;

// Threading section
extern int _comm_thread_load_exchange_happend;

// variables to indicate when it is save to break out of taskwait
extern std::mutex _mtx_taskwait;
extern std::atomic<int> _flag_comm_threads_sleeping;
extern int _num_threads_involved_in_taskwait;
// extern int _num_threads_entered_taskwait;
extern std::atomic<int32_t> _num_threads_entered_taskwait;
extern std::atomic<int32_t> _num_threads_idle;
extern int _num_ranks_not_completely_idle;

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

int exit_condition_met(int print);

#ifdef __cplusplus
}
#endif
#endif
