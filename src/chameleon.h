// Chameleon Header
#ifndef _CHAMELEON_H_
#define _CHAMELEON_H_

#include <cstdint>
#include <inttypes.h>
#include <list>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#ifndef DPxMOD
#define DPxMOD "0x%0*" PRIxPTR
#endif

#ifndef DPxPTR
#define DPxPTR(ptr) ((int)(2*sizeof(uintptr_t))), ((uintptr_t) (ptr))
#endif

#ifndef DBP
#ifdef CHAM_DEBUG
#define DBP( ... )                                                      \
  {                                                                     \
    fprintf(stderr, "ChameleonLib T#%d: --> ", chameleon_comm_rank);    \
    fprintf(stderr, __VA_ARGS__);                                       \
  }
#else
#define DBP( ... ) { }
#endif
#endif

extern int chameleon_comm_rank;
extern int chameleon_comm_size;

// TODO: fix that to have only one place where that is defined
// copy of OpenMP target argument types
enum chameleon_tgt_map_type {
  // No flags
  CHAM_OMP_TGT_MAPTYPE_NONE            = 0x000,
  // copy data from host to device
  CHAM_OMP_TGT_MAPTYPE_TO              = 0x001,
  // copy data from device to host
  CHAM_OMP_TGT_MAPTYPE_FROM            = 0x002,
  // copy regardless of the reference count
  CHAM_OMP_TGT_MAPTYPE_ALWAYS          = 0x004,
  // force unmapping of data
  CHAM_OMP_TGT_MAPTYPE_DELETE          = 0x008,
  // map the pointer as well as the pointee
  CHAM_OMP_TGT_MAPTYPE_PTR_AND_OBJ     = 0x010,
  // pass device base address to kernel
  CHAM_OMP_TGT_MAPTYPE_TARGET_PARAM    = 0x020,
  // return base device address of mapped data
  CHAM_OMP_TGT_MAPTYPE_RETURN_PARAM    = 0x040,
  // private variable - not mapped
  CHAM_OMP_TGT_MAPTYPE_PRIVATE         = 0x080,
  // copy by value - not mapped
  CHAM_OMP_TGT_MAPTYPE_LITERAL         = 0x100,
  // mapping is implicit
  CHAM_OMP_TGT_MAPTYPE_IMPLICIT        = 0x200,
  // member of struct, member given by 16 MSBs - 1
  CHAM_OMP_TGT_MAPTYPE_MEMBER_OF       = 0xffff000000000000
};

enum chameleon_result_types {
    CHAM_SUCCESS = 0,
    CHAM_FAILURE = 1,

    CHAM_REMOTE_TASK_NONE = 2,
    CHAM_REMOTE_TASK_SUCCESS = 3,
    CHAM_REMOTE_TASK_FAILURE = 4    
};

enum chameleon_task_status {
    CHAM_TASK_STATUS_OPEN = 0,
    CHAM_TASK_STATUS_PROCESSING = 1,
    CHAM_TASK_STATUS_DONE = 2
};

struct TargetTaskEntryTy {
    intptr_t tgt_entry_ptr;
    
    // we need index of image here as well since pointers are not matching for other ranks
    int32_t idx_image = 0;
    ptrdiff_t entry_image_offset = 0;

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
    std::vector<void *> arg_tgt_converted_pointers;

    // Some special settings for stolen tasks
    int32_t status = CHAM_TASK_STATUS_OPEN;
    int32_t source_mpi_rank = 0;
    int32_t source_mpi_tag = 0;

    // Constructor 1: Called when creating new task during decoding
    TargetTaskEntryTy() {

    }

    // Constructor 2: Called from libomptarget plugin to create a new task
    TargetTaskEntryTy(
        void *p_tgt_entry_ptr, 
        void **p_tgt_args, 
        ptrdiff_t *p_tgt_offsets, 
        int64_t *p_tgt_arg_types, 
        int32_t p_arg_num) {

            tgt_entry_ptr = (intptr_t) p_tgt_entry_ptr;
            arg_num = p_arg_num;

            // resize vectors
            arg_hst_pointers.resize(arg_num);
            arg_sizes.resize(arg_num);

            arg_types.resize(arg_num);
            arg_tgt_pointers.resize(arg_num);
            arg_tgt_offsets.resize(arg_num);
            arg_tgt_converted_pointers.resize(arg_num);

            for(int i = 0; i < arg_num; i++) {
                arg_tgt_pointers[i] = p_tgt_args[i];
                arg_tgt_offsets[i] = p_tgt_offsets[i];
                arg_types[i] = p_tgt_arg_types[i];
                
                // calculate converted pointers that include offset
                arg_tgt_converted_pointers[i] = (void *)((intptr_t)arg_tgt_pointers[i] + arg_tgt_offsets[i]);
            }
        }
    
    void ReSizeArrays(int32_t num_args) {
        arg_hst_pointers.resize(num_args);
        arg_sizes.resize(num_args);
        arg_types.resize(num_args);
    }

    int HasAtLeastOneOutput();
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
    int32_t ref_count;

    OffloadingDataEntryTy() {
        tgt_ptr = nullptr;
        hst_ptr = nullptr;
        size = 0;
        ref_count = 1;
    }

    OffloadingDataEntryTy(void *p_tgt_ptr, void *p_hst_ptr, int64_t p_size) {
        tgt_ptr = p_tgt_ptr;
        hst_ptr = p_hst_ptr;
        size = p_size;
        ref_count = 1;
    }
};

#ifdef __cplusplus
extern "C" {
#endif
// ================================================================================
// External functions (that can be called from source code or libomptarget)
// ================================================================================
int32_t chameleon_init();

int32_t chameleon_set_image_base_address(int idx_image, intptr_t base_address);

int32_t chameleon_finalize();

int32_t chameleon_distributed_taskwait();

int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size);

int32_t chameleon_add_task(TargetTaskEntryTy *task);

TargetTaskEntryTy* chameleon_pop_task();

#ifdef __cplusplus
}
#endif

#endif
