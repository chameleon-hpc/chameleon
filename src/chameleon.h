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

// TODO: fix that to have only one place where that is defined
// copy of OpenMP target argument types
enum chameleon_tgt_map_type {
  // No flags
  CH_OMP_TGT_MAPTYPE_NONE            = 0x000,
  // copy data from host to device
  CH_OMP_TGT_MAPTYPE_TO              = 0x001,
  // copy data from device to host
  CH_OMP_TGT_MAPTYPE_FROM            = 0x002,
  // copy regardless of the reference count
  CH_OMP_TGT_MAPTYPE_ALWAYS          = 0x004,
  // force unmapping of data
  CH_OMP_TGT_MAPTYPE_DELETE          = 0x008,
  // map the pointer as well as the pointee
  CH_OMP_TGT_MAPTYPE_PTR_AND_OBJ     = 0x010,
  // pass device base address to kernel
  CH_OMP_TGT_MAPTYPE_TARGET_PARAM    = 0x020,
  // return base device address of mapped data
  CH_OMP_TGT_MAPTYPE_RETURN_PARAM    = 0x040,
  // private variable - not mapped
  CH_OMP_TGT_MAPTYPE_PRIVATE         = 0x080,
  // copy by value - not mapped
  CH_OMP_TGT_MAPTYPE_LITERAL         = 0x100,
  // mapping is implicit
  CH_OMP_TGT_MAPTYPE_IMPLICIT        = 0x200,
  // member of struct, member given by 16 MSBs - 1
  CH_OMP_TGT_MAPTYPE_MEMBER_OF       = 0xffff000000000000
};

enum chameleon_result_types {
    CHAM_SUCCESS = 0,
    CHAM_FAILURE = 1
};

struct OffloadingTaskEntryTy {
    void *tgt_entry_ptr;
    void **tgt_args;
    ptrdiff_t *tgt_offsets;
    int64_t *tgt_arg_types;
    int32_t arg_num;

    OffloadingTaskEntryTy() {
        tgt_entry_ptr = nullptr;
        tgt_args = nullptr;
        tgt_offsets = nullptr;
        tgt_arg_types = nullptr;
        arg_num = 0;
    }

    OffloadingTaskEntryTy(
        void *p_tgt_entry_ptr, 
        void **p_tgt_args, 
        ptrdiff_t *p_tgt_offsets, 
        int64_t *p_tgt_arg_types, 
        int32_t p_arg_num) {
            arg_num = p_arg_num;
            tgt_entry_ptr = p_tgt_entry_ptr;

            tgt_args = (void**) malloc(sizeof(void*) * p_arg_num);
            tgt_offsets = (ptrdiff_t *) malloc(sizeof(ptrdiff_t) * p_arg_num);
            tgt_arg_types = (int64_t *) malloc(sizeof(int64_t) * p_arg_num);

            for(int i = 0; i < p_arg_num; i++){
                tgt_args[i] = p_tgt_args[i];
                tgt_offsets[i] = p_tgt_offsets[i];
                tgt_arg_types[i] = p_tgt_arg_types[i];
            }
        }
    
    // ~OffloadingTaskEntryTy() {
    //     free(tgt_args);
    //     free(tgt_offsets);
    //     free(tgt_arg_types);
    // }
};

struct OffloadingDataEntryTy {
    void *tgt_ptr;
    void *hst_ptr;
    int64_t size;
    int32_t ref_count;

    // does it make sense to put these two here? (might be used in multiple target regions)
    // int32_t send_back;
    // int32_t is_implicit;

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
    // ~OffloadingDataEntryTy() {
    //     tgt_ptr = nullptr;
    //     hst_ptr = nullptr;
    //     size = 0;
    //     ref_count = 1;
    // }
};

#ifdef __cplusplus
extern "C" {
#endif

int32_t chameleon_init();

int32_t chameleon_finalize();

int32_t chameleon_distributed_taskwait();

int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size);

int32_t chameleon_add_task(OffloadingTaskEntryTy task);

#ifdef __cplusplus
}
#endif

#endif
