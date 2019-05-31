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
// communicator for task cancellation
extern MPI_Comm chameleon_comm_cancel;

extern RequestManager request_manager_send;
extern RequestManager request_manager_receive;
extern RequestManager request_manager_cancel;

extern int chameleon_comm_rank;
extern int chameleon_comm_size;

extern std::vector<intptr_t> _image_base_addresses;

// list with local task entries
// these can either be executed here or offloaded to a different rank
extern thread_safe_task_list_t _local_tasks;
extern std::atomic<int32_t> _num_local_tasks_outstanding;

// list with stolen task entries that should be executed
extern thread_safe_task_list_t _stolen_remote_tasks;
extern std::atomic<int32_t> _num_stolen_tasks_outstanding;

// list of replicated (i.e. offloaded) tasks
// they can be executed either on the remote rank or on the local rank
extern thread_safe_task_list_t _replicated_tasks;
extern std::atomic<int32_t> _num_replicated_tasks_outstanding;

// list with stolen task entries that need output data transfer
extern thread_safe_task_list_t _stolen_remote_tasks_send_back;

// map that maps tag ids back to local tasks that have been offloaded and expect result data
extern thread_safe_task_map_t _map_offloaded_tasks_with_outputs;
// map that maps tag ids back to stolen tasks
extern thread_safe_task_map_t _map_tag_to_stolen_task;
// mapping of all active task ids and task
extern thread_safe_task_map_t _map_overall_tasks;

// for now use a single mutex for box info
extern std::mutex _mtx_load_exchange;
// ====== Info about outstanding jobs (local & stolen & offloaded (communication)) ======
extern std::vector<int32_t> _outstanding_jobs_ranks;
extern std::atomic<int32_t> _outstanding_jobs_local;
extern std::atomic<int32_t> _outstanding_jobs_sum;
// ====== Info about real load that is open or is beeing processed ======
extern std::vector<int32_t> _load_info_ranks;

// list that holds task ids (created at the current rank) that are not finsihed yet
extern thread_safe_list_t<TYPE_TASK_ID> _unfinished_locally_created_tasks;

// Threading section
extern std::atomic<int> _comm_thread_load_exchange_happend;

// variables to indicate when it is save to break out of taskwait
extern std::mutex _mtx_taskwait;
extern std::atomic<int> _num_threads_involved_in_taskwait;
extern std::atomic<int32_t> _num_threads_idle;
extern std::atomic<int> _num_ranks_not_completely_idle;

#ifdef __cplusplus
extern "C" {
#endif

void print_arg_info(std::string prefix, cham_migratable_task_t *task, int idx);

void print_arg_info_w_tgt(std::string prefix, cham_migratable_task_t *task, int idx);

void cancel_offloaded_task(cham_migratable_task_t *task);

void* comm_thread_action(void *arg);

int32_t start_communication_threads();

int32_t stop_communication_threads();

int32_t put_comm_threads_to_sleep();

void trigger_update_outstanding();

int exit_condition_met(int from_taskwait, int print);

#ifdef __cplusplus
}
#endif
#endif
