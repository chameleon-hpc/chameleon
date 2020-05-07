#include "chameleon_strategies.h"
#include "commthread.h" 
#include "chameleon_common.h"
 
#include <numeric>
#include <algorithm>
#include <cassert>

#pragma region Local Helpers
template <typename T>
std::vector<size_t> sort_indexes(const std::vector<T> &v) {

  // initialize original index locations
  std::vector<size_t> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);

  // sort indexes based on comparing values in v
  std::sort(idx.begin(), idx.end(),
       [&v](size_t i1, size_t i2) {return v[i1] < v[i2];});

  return idx;
}
#pragma endregion Local Helpers

#pragma region Strategies
void compute_num_tasks_to_offload(std::vector<int32_t>& tasksToOffloadPerRank, std::vector<int32_t>& loadInfoRanks, int32_t num_tasks_local, int32_t num_tasks_stolen) {
#if OFFLOADING_STRATEGY_AGGRESSIVE
    int input_r = 0, input_l = 0;
    int output_r = 0, output_l = 0;

    int total_l = 0;
    total_l =std::accumulate(&loadInfoRanks[0], &loadInfoRanks[chameleon_comm_size], total_l);  
    int avg_l = total_l / chameleon_comm_size;
  
    input_l = loadInfoRanks[input_r];
    output_l = loadInfoRanks[output_r];

    while(output_r<chameleon_comm_size) {
        int target_load_out = avg_l; //TODO: maybe use this to compute load dependent on characteristics of target rank (slow node..)
        int target_load_in = avg_l;

        while(output_l<target_load_out) {
            int diff_l = target_load_out-output_l;

            if(output_r==input_r) {
                input_r++; 
                input_l = loadInfoRanks[input_r];
                continue;
            }

            int moveable = input_l-target_load_in;
            if(moveable>0) {
                int inc_l = std::min( diff_l, moveable );
                output_l += inc_l;
                input_l -= inc_l;
 
                if(input_r==chameleon_comm_rank) {
                    tasksToOffloadPerRank[output_r]= inc_l;
                }
            }
       
            if(input_l <=target_load_in ) {
                input_r++;
                if(input_r<chameleon_comm_size) {
                    input_l = loadInfoRanks[input_r];
                    target_load_in = avg_l;
                }
            }
        }
        output_r++;
        if(output_r<chameleon_comm_size)
            output_l = loadInfoRanks[output_r];
    }
#else
    // sort load and idx by load
    std::vector<size_t> tmp_sorted_idx = sort_indexes(loadInfoRanks);

    double min_val                  = (double) loadInfoRanks[tmp_sorted_idx[0]];
    double max_val                  = (double) loadInfoRanks[tmp_sorted_idx[chameleon_comm_size-1]];
    double cur_load                 = (double) loadInfoRanks[chameleon_comm_rank];
    
    double ratio_lb                 = 0.0; // 1 = high imbalance, 0 = no imbalance
    if (max_val > 0) {
        ratio_lb = (double)(max_val-min_val) / (double)max_val;
    }
#if !FORCE_MIGRATION
    // check absolute condition
    if((cur_load-min_val) < MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION)
        return;

    if(ratio_lb >= MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION) {
#else
    if(true) {
#endif
        // determine index
        int pos = std::find(tmp_sorted_idx.begin(), tmp_sorted_idx.end(), chameleon_comm_rank) - tmp_sorted_idx.begin();
#if !FORCE_MIGRATION
        // only offload if on the upper side
        if((pos) >= ((double)chameleon_comm_size/2.0))
        {
#endif
            int other_pos       = chameleon_comm_size-pos-1;
            int other_idx       = tmp_sorted_idx[other_pos];
            double other_val    = (double) loadInfoRanks[other_idx];

            double cur_diff = cur_load-other_val;
#if !FORCE_MIGRATION
            // check absolute condition
            if(cur_diff < MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION)
                return;
            double ratio = cur_diff / (double)cur_load;
            if(other_val < cur_load && ratio >= MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION) {
#endif
                int num_tasks = (int)(cur_diff * PERCENTAGE_DIFF_TASKS_TO_MIGRATE);
                if(num_tasks < 1)
                    num_tasks = 1;
                // RELP("Migrating\t%d\ttasks to rank:\t%d\tload:\t%f\tload_victim:\t%f\tratio:\t%f\tdiff:\t%f\n", num_tasks, other_idx, cur_load, other_val, ratio, cur_diff);
                tasksToOffloadPerRank[other_idx] = num_tasks;
#if !FORCE_MIGRATION
            }
        }
#endif
    }
#endif
}

// implements default replication strategy where neighbouring ranks logically have some "overlapping tasks"
 cham_t_replication_info_t * compute_num_tasks_to_replicate(  std::vector<int32_t>& loadInfoRanks, int32_t num_tasks_local, int32_t *num_replication_infos ) {

    double alpha = 0;
    int myLeft = (chameleon_comm_rank-1 + chameleon_comm_size)%chameleon_comm_size;
    int myRight = (chameleon_comm_rank+1 + chameleon_comm_size)%chameleon_comm_size;
    
    assert(num_tasks_local>=0);

    int num_neighbours = 0;
    if(myLeft>=0) num_neighbours++;
    if(myRight<chameleon_comm_size) num_neighbours++;
    cham_t_replication_info_t *replication_infos = (cham_t_replication_info_t*) malloc(sizeof(cham_t_replication_info_t)*num_neighbours);

    alpha = MAX_PERCENTAGE_REPLICATED_TASKS/num_neighbours;
    assert(alpha>=0);

    int32_t cnt = 0;

	if(myLeft>=0) {
	    //printf("alpha %f, num_tasks_local %d\n", alpha, num_tasks_local);
	    int num_tasks = num_tasks_local*alpha;
            assert(num_tasks>=0);
	    int *replication_ranks = (int*) malloc(sizeof(int)*1);
	    replication_ranks[0] = myLeft;
		cham_t_replication_info_t info = cham_t_replication_info_create(num_tasks, 1, replication_ranks);
		replication_infos[cnt++] = info;
	}
	if(myRight<chameleon_comm_size) {
	    int num_tasks = num_tasks_local*alpha;
            assert(num_tasks>=0);
	    int *replication_ranks = (int*) malloc(sizeof(int)*1);
	    replication_ranks[0] = myRight;
	    cham_t_replication_info_t info = cham_t_replication_info_create(num_tasks, 1, replication_ranks);
	    replication_infos[cnt++] = info;
	}
	*num_replication_infos = cnt;
	return replication_infos;
}

int32_t get_default_load_information_for_rank(TYPE_TASK_ID* local_task_ids, int32_t num_tasks_local, TYPE_TASK_ID* local_rep_task_ids, int32_t num_tasks_local_rep, TYPE_TASK_ID* stolen_task_ids, int32_t num_tasks_stolen, TYPE_TASK_ID* stolen_task_ids_rep, int32_t num_tasks_stolen_rep) {
    // simply return number of tasks in queue
    // int32_t num_ids = num_tasks_local + num_tasks_stolen;
    int32_t num_ids;
    assert(num_tasks_stolen_rep>=0);
    assert(num_tasks_stolen>=0);
    assert(num_tasks_local_rep>=0);
    assert(num_tasks_local>=0);

    num_ids = num_tasks_local + num_tasks_local_rep;  //Todo: include replicated tasks which are "in flight"
#if CHAM_REPLICATION_MODE==1
    num_ids += num_tasks_stolen + num_tasks_stolen_rep;
#else
    num_ids += num_tasks_stolen;
#endif
//    int tmp_num_stolen = _num_remote_tasks_outstanding.load();
//    if (tmp_num_stolen > num_tasks_stolen)
//        num_ids = num_tasks_local + tmp_num_stolen;
//    else
//        num_ids = num_tasks_local + num_tasks_stolen;
    return num_ids;
}
#pragma endregion Strategies
