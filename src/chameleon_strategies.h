// Load balancing strategies header
#ifndef _CHAMELEON_STRATEGIES_H_
#define _CHAMELEON_STRATEGIES_H_

#include "chameleon.h"
#include <vector>
#include <stdint.h>
#include <stddef.h>

void compute_num_tasks_to_offload( std::vector<int32_t>& tasks_to_offload_per_rank, std::vector<int32_t>& load_info_ranks, int32_t num_tasks_local, int32_t num_tasks_stolen);

void compute_num_tasks_to_replicate( std::vector<cham_replication_info_t>& replication_infos, std::vector<int32_t>& loadInfoRanks, int32_t num_tasks_local);

extern int32_t get_default_load_information_for_rank(TYPE_TASK_ID* local_task_ids, int32_t num_tasks_local, TYPE_TASK_ID* local_rep_task_ids, int32_t num_tasks_local_rep, TYPE_TASK_ID* stolen_task_ids, int32_t num_tasks_stolen, TYPE_TASK_ID* stolen_task_ids_rep, int32_t num_tasks_stolen_rep);

#endif
