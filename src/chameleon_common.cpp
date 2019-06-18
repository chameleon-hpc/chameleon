#ifndef _CHAMELEON_COMMON_CPP_
#define _CHAMELEON_COMMON_CPP_

#include "chameleon_common.h"
#include <stdint.h>
#include <limits.h>

#pragma region Variables
// atomic counter for task ids
std::atomic<int32_t> _thread_counter(0);
__thread int32_t __ch_gtid = -1;

ch_thread_data_t*   __thread_data;
ch_rank_data_t      __rank_data;

std::atomic<int> _tracing_enabled(1);
std::atomic<int> _num_sync_cycle(0);

// ============================================================ 
// config values defined through environment variables
// ============================================================
// general settings for migration
std::atomic<double> MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION(2);
std::atomic<double> MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE(1);

// settings to manipulate default migration strategy
std::atomic<double> MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION(2);
std::atomic<double> MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION(0.05);
std::atomic<double> PERCENTAGE_DIFF_TASKS_TO_MIGRATE(1);
std::atomic<int> OMP_NUM_THREADS_VAR(1);

// settings to enable / disable tracing only for specific range of synchronization cycles
std::atomic<int> ENABLE_TRACE_FROM_SYNC_CYCLE(-1);
std::atomic<int> ENABLE_TRACE_TO_SYNC_CYCLE(INT_MAX);
#pragma endregion

#pragma region Functions
int32_t __ch_get_gtid() {
    if(__ch_gtid != -1)
        return __ch_gtid;

    __ch_gtid = _thread_counter++;
    
    // init thread_data here
    __thread_data[__ch_gtid].os_thread_id = syscall(SYS_gettid);
    __thread_data[__ch_gtid].current_task = nullptr;

    return __ch_gtid;
}

cham_migratable_task_t::cham_migratable_task_t(
    void *p_tgt_entry_ptr, 
    void **p_tgt_args, 
    ptrdiff_t *p_tgt_offsets, 
    int64_t *p_tgt_arg_types, 
    int32_t p_arg_num) 
  : result_in_progress(false), is_replicated_task(0) {
        
    // generate a unique task id
    TYPE_TASK_ID tmp_counter = ++_task_id_counter;
    // int tmp_rank = chameleon_comm_rank;
    task_id = (chameleon_comm_rank << 24) | (tmp_counter);
    // DBP("cham_migratable_task_t - Created task with (task_id=%ld)\n", task_id);

    tgt_entry_ptr = (intptr_t) p_tgt_entry_ptr;
    arg_num = p_arg_num;

    // resize vectors
    arg_hst_pointers.resize(arg_num);
    arg_sizes.resize(arg_num);

    arg_types.resize(arg_num);
    arg_tgt_pointers.resize(arg_num);
    arg_tgt_offsets.resize(arg_num);

    for(int i = 0; i < arg_num; i++) {
        arg_tgt_pointers[i] = p_tgt_args[i];
        arg_tgt_offsets[i] = p_tgt_offsets[i];
        arg_types[i] = p_tgt_arg_types[i];
    }
}

cham_migratable_task_t::cham_migratable_task_t(
    void *p_tgt_entry_ptr,
    void **p_tgt_args,
    ptrdiff_t *p_tgt_offsets,
    int64_t *p_tgt_arg_types,
    int32_t p_arg_num,
    int num_replicating_ranks,
    int *rep_ranks) 
  : result_in_progress(false), is_replicated_task(0) {
 
    //Todo: avoid code duplication and extract init() function

    // generate a unique task id
    TYPE_TASK_ID tmp_counter = ++_task_id_counter;
    // int tmp_rank = chameleon_comm_rank;
    task_id = (chameleon_comm_rank << 24) | (tmp_counter);
    // DBP("cham_migratable_task_t - Created task with (task_id=%ld)\n", task_id);

    tgt_entry_ptr = (intptr_t) p_tgt_entry_ptr;
    arg_num = p_arg_num;

    // resize vectors
    arg_hst_pointers.resize(arg_num);
    arg_sizes.resize(arg_num);

    arg_types.resize(arg_num);
    arg_tgt_pointers.resize(arg_num);
    arg_tgt_offsets.resize(arg_num);

    for(int i = 0; i < arg_num; i++) {
        arg_tgt_pointers[i] = p_tgt_args[i];
        arg_tgt_offsets[i] = p_tgt_offsets[i];
        arg_types[i] = p_tgt_arg_types[i];
    }

    if(num_replicating_ranks)
      is_replicated_task = 1;

    for(int i = 0; i < num_replicating_ranks; i++) {
        replicating_ranks.push_back( rep_ranks[i] );    
    }
}

void cham_migratable_task_t::ReSizeArrays(int32_t num_args) {
    arg_hst_pointers.resize(num_args);
    arg_sizes.resize(num_args);
    arg_types.resize(num_args);
    arg_tgt_offsets.resize(num_args);
}

int cham_migratable_task_t::HasAtLeastOneOutput() {
    for(int i = 0; i < this->arg_num; i++) {
        if(this->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM)
            return 1;
    }
    return 0;
}
#pragma endregion
#endif

