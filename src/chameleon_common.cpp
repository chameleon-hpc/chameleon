#ifndef _CHAMELEON_COMMON_CPP_
#define _CHAMELEON_COMMON_CPP_

#include "chameleon_common.h"
#include "chameleon.h"

#pragma region Structs
struct TargetTaskEntryTy {
    // target entry point / function that holds the code that should be executed
    intptr_t tgt_entry_ptr;
    // we need index of image here as well since pointers are not matching for other ranks
    int32_t idx_image = 0;
    ptrdiff_t entry_image_offset = 0;

    // task id (unique id that combines the host rank and a unique id per rank)
    int32_t task_id;

    // number of arguments that should be passed to "function call" for target region
    int32_t arg_num;

    // host pointers will be used for transfer execution target region
    std::vector<void *> arg_hst_pointers;
    std::vector<int64_t> arg_sizes;
    std::vector<int64_t> arg_types;

    // target pointers will just be used at sender side for host pointer lookup 
    // and freeing of entries in data entry table
    std::vector<void *> arg_tgt_pointers;
    std::vector<ptrdiff_t> arg_tgt_offsets;

    // Some special settings for stolen tasks
    int32_t source_mpi_rank = 0;
    int32_t source_mpi_tag = 0;
    int32_t target_mpi_rank = -1;

    // Mutex for either execution or receiving back/cancellation of a replicated task
    std::atomic<bool> sync_commthread_lock;

    // Constructor 1: Called when creating new task during decoding
    TargetTaskEntryTy() : sync_commthread_lock(false) {
        // here we dont need to give a task id in that case because it should be transfered from source
    }

    // Constructor 2: Called from libomptarget plugin to create a new task
    TargetTaskEntryTy(
        void *p_tgt_entry_ptr, 
        void **p_tgt_args, 
        ptrdiff_t *p_tgt_offsets, 
        int64_t *p_tgt_arg_types, 
        int32_t p_arg_num) : sync_commthread_lock(false) {
            //sync_commthread_lock= false; 

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
    
    void ReSizeArrays(int32_t num_args) {
        arg_hst_pointers.resize(num_args);
        arg_sizes.resize(num_args);
        arg_types.resize(num_args);
        arg_tgt_offsets.resize(num_args);
    }

    int HasAtLeastOneOutput() {
        for(int i = 0; i < this->arg_num; i++) {
            if(this->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM)
                return 1;
        }
        return 0;
    }
};

struct OffloadEntryTy {
    TargetTaskEntryTy *task_entry;
    int32_t target_rank;

    OffloadEntryTy(TargetTaskEntryTy *par_task_entry, int32_t par_target_rank) {
        task_entry = par_task_entry;
        target_rank = par_target_rank;
    }
};

struct OffloadingDataEntryTy {
    void *tgt_ptr;
    void *hst_ptr;
    int64_t size;

    OffloadingDataEntryTy() {
        tgt_ptr = nullptr;
        hst_ptr = nullptr;
        size = 0;
    }

    OffloadingDataEntryTy(void *p_tgt_ptr, void *p_hst_ptr, int64_t p_size) {
        tgt_ptr = p_tgt_ptr;
        hst_ptr = p_hst_ptr;
        size = p_size;
    }
};

struct MapEntry{
    void *valptr;
    size_t size;
    int type;
};
#pragma endregion
#endif
