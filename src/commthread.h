// Communication thread header
#ifndef _COMMTHREAD_H_
#define _COMMTHREAD_H_

#include <mpi.h>

#include "chameleon.h"

// communicator for remote task requests
extern MPI_Comm chameleon_comm;
// communicator for sending back mapped values
extern MPI_Comm chameleon_comm_mapped;
extern int chameleon_comm_rank;
extern int chameleon_comm_size;

extern std::vector<intptr_t> _image_base_addresses;

// list with data that has been mapped in map clauses
extern std::mutex _mtx_data_entry;
extern std::list<OffloadingDataEntryTy*> _data_entries;

// list with local task entries
// these can either be executed here or offloaded to a different rank
extern std::mutex _mtx_local_tasks;
extern std::list<TargetTaskEntryTy*> _local_tasks;

extern int32_t _all_local_tasks_done;

// list with stolen task entries that should be executed
extern std::mutex _mtx_stolen_remote_tasks;
extern std::list<TargetTaskEntryTy*> _stolen_remote_tasks;

// entries that should be offloaded to specific ranks
extern std::mutex _mtx_offload_entries;
extern std::list<OffloadEntryTy*> _offload_entries;

// Load Information (temporary here: number of tasks)
extern std::mutex _mtx_local_load_info;
extern int32_t _local_load_info;

extern std::mutex _mtx_complete_load_info;
extern int32_t *_complete_load_info;
extern int32_t _sum_complete_load_info;

#ifdef __cplusplus
extern "C" {
#endif

int32_t offload_task_to_rank(OffloadEntryTy *entry);

void* receive_remote_tasks(void* arg);

int32_t send_back_remote_task_data();

int32_t start_communication_threads();

int32_t stop_communication_threads();

#ifdef __cplusplus
}
#endif
#endif