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
void computeNumTasksToOffload( std::vector<int32_t>& tasksToOffloadPerRank, std::vector<int32_t>& loadInfoRanks ) {
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
    std::vector<size_t> tmp_sorted_idx = sort_indexes(loadInfoRanks);
    int min_val = loadInfoRanks[tmp_sorted_idx[0]];
    int max_val = loadInfoRanks[tmp_sorted_idx[chameleon_comm_size-1]];

    int cur_load = loadInfoRanks[chameleon_comm_rank];
    
    if(max_val > min_val) {
    // determine index
        int pos = std::find(tmp_sorted_idx.begin(), tmp_sorted_idx.end(), chameleon_comm_rank) - tmp_sorted_idx.begin();
        // only offload if on the upper side
        if((pos+1) >= ((double)chameleon_comm_size/2.0))
        {
            int other_pos = chameleon_comm_size-pos;
            // need to adapt in case of even number
            if(chameleon_comm_size % 2 == 0)
                other_pos--;
            int other_idx = tmp_sorted_idx[other_pos];
            int other_val = loadInfoRanks[other_idx];

            // calculate ration between those two and just move if over a certain threshold
#if !FORCE_MIGRATION
            double ratio = (double)(cur_load-other_val) / (double)cur_load;
            if(other_val < cur_load && ratio > 0.5) {
#endif
                tasksToOffloadPerRank[other_idx] = 1;
#if !FORCE_MIGRATION
            }
#endif
        } 
    }
#endif
}

int32_t getDefaultLoadInformationForRank(TYPE_TASK_ID* local_task_ids, int32_t num_ids_local, TYPE_TASK_ID* stolen_task_ids, int32_t num_ids_stolen) {
    // simply return number of tasks in queue
    int32_t num_ids = num_ids_local + num_ids_stolen;
    return num_ids;
}
#pragma endregion Strategies
