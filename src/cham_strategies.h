// Load balancing strategies header
#ifndef _CHAMELEON_STRATEGIES_H_
#define _CHAMELEON_STRATEGIES_H_

#include "chameleon.h"
#include <vector>
#include <stdint.h>
#include <stddef.h>

void computeNumTasksToOffload( std::vector<int32_t>& tasksToOffloadPerRank, std::vector<int32_t>& loadInfoRanks );

extern int32_t getDefaultLoadInformationForRank(TYPE_TASK_ID* local_task_ids, int32_t num_ids_local, TYPE_TASK_ID* stolen_task_ids, int32_t num_ids_stolen);

#endif
