#include "cham_strategies.h"
#include "commthread.h" 
#include "chameleon_common.h"
 
#include <numeric>
#include <algorithm>

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
void computeNumTasksToOffload( std::vector<int32_t>& tasksToOffloadPerRank, std::vector<int32_t>& loadInfoRanks, int32_t num_tasks_local, int32_t num_tasks_stolen) {
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
    static double min_abs_imbalance_before_migration = -1;
    if(min_abs_imbalance_before_migration == -1) {
        // try to load it once
        char *min_abs_balance = std::getenv("MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION");
        if(min_abs_balance) {
            min_abs_imbalance_before_migration = std::atof(min_abs_balance);
        } else {
            min_abs_imbalance_before_migration = 2;
        }
    }

    static double min_rel_imbalance_before_migration = -1;
    if(min_rel_imbalance_before_migration == -1) {
        // try to load it once
        char *min_rel_balance = std::getenv("MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION");
        if(min_rel_balance) {
            min_rel_imbalance_before_migration = std::atof(min_rel_balance);
        } else {
            // default relative threshold
            min_rel_imbalance_before_migration = 0.05;
        }
    }

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
    if((cur_load-min_val) < min_abs_imbalance_before_migration)
        return;

    if(ratio_lb >= min_rel_imbalance_before_migration) {
#else
    if(true) {
#endif
        // determine index
        int pos = std::find(tmp_sorted_idx.begin(), tmp_sorted_idx.end(), chameleon_comm_rank) - tmp_sorted_idx.begin();
        // only offload if on the upper side
        if((pos+1) >= ((double)chameleon_comm_size/2.0))
        {
            int other_pos       = chameleon_comm_size-pos;
            // need to adapt in case of even number
            if(chameleon_comm_size % 2 == 0)
                other_pos--;
            int other_idx       = tmp_sorted_idx[other_pos];
            double other_val    = (double) loadInfoRanks[other_idx];

#if !FORCE_MIGRATION
            // check absolute condition
            if((cur_load-other_val) < min_abs_imbalance_before_migration)
                return;
            double ratio = (double)(cur_load-other_val) / (double)cur_load;
            if(other_val < cur_load && ratio >= min_rel_imbalance_before_migration) {
#endif
                tasksToOffloadPerRank[other_idx] = 1;
#if !FORCE_MIGRATION
            }
#endif
        }
    }
#endif
}

int32_t getDefaultLoadInformationForRank(TYPE_TASK_ID* local_task_ids, int32_t num_tasks_local, TYPE_TASK_ID* stolen_task_ids, int32_t num_tasks_stolen) {
    // simply return number of tasks in queue
    int32_t num_ids = num_tasks_local + num_tasks_stolen;
    return num_ids;
}
#pragma endregion Strategies
