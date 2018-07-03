// Communication thread header
#ifndef _COMMTHREAD_H_
#define _COMMTHREAD_H_

#include <mpi.h>

#include "chameleon.h"

extern MPI_Comm chameleon_comm;
extern int chameleon_comm_rank;
extern int chameleon_comm_size;

extern std::mutex _mtx_data_entry;
extern std::list<OffloadingDataEntryTy> _data_entries;
extern std::mutex _mtx_tasks;
extern std::list<OffloadingTaskEntryTy> _tasks;

#ifdef __cplusplus
extern "C" {
#endif

int32_t offload_task_to_rank(OffloadingTaskEntryTy task, int rank);

#ifdef __cplusplus
}
#endif
#endif