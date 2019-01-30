#ifndef _CHAMELEON_COMMON_CPP_
#define _CHAMELEON_COMMON_CPP_

#include "chameleon_common.h"

#pragma region Variables
// atomic counter for task ids
std::atomic<int32_t> _thread_counter(0);
__thread int32_t __ch_gtid = -1;

ch_thread_data_t*   __thread_data;
ch_rank_data_t      __rank_data;
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

TargetTaskEntryTy::TargetTaskEntryTy(
    void *p_tgt_entry_ptr, 
    void **p_tgt_args, 
    ptrdiff_t *p_tgt_offsets, 
    int64_t *p_tgt_arg_types, 
    int32_t p_arg_num) {
        
    // generate a unique task id
    int tmp_counter = ++_task_id_counter;
    // int tmp_rank = chameleon_comm_rank;
    task_id = (chameleon_comm_rank << 16) | (tmp_counter);
    // DBP("TargetTaskEntryTy - Created task with (task_id=%d)\n", task_id);

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

void TargetTaskEntryTy::ReSizeArrays(int32_t num_args) {
    arg_hst_pointers.resize(num_args);
    arg_sizes.resize(num_args);
    arg_types.resize(num_args);
    arg_tgt_offsets.resize(num_args);
}

int TargetTaskEntryTy::HasAtLeastOneOutput() {
    for(int i = 0; i < this->arg_num; i++) {
        if(this->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM)
            return 1;
    }
    return 0;
}
#pragma endregion
#endif