// Chameleon Header
#ifndef _CHAMELEON_H_
#define _CHAMELEON_H_

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <omp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>

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

enum chameleon_device_ids {
  CHAMELEON_HOST    = 1001,
  CHAMELEON_MPI     = 1002,
};

enum chameleon_result_types {
    CHAM_SUCCESS = 0,
    CHAM_FAILURE = 1,

    CHAM_LOCAL_TASK_NONE = 2,
    CHAM_LOCAL_TASK_SUCCESS = 3,
    CHAM_LOCAL_TASK_FAILURE = 4,

    CHAM_REMOTE_TASK_NONE = 5,
    CHAM_REMOTE_TASK_SUCCESS = 6,
    CHAM_REMOTE_TASK_FAILURE = 7    
};

enum chameleon_task_status {
    CHAM_TASK_STATUS_OPEN = 0,
    CHAM_TASK_STATUS_PROCESSING = 1,
    CHAM_TASK_STATUS_DONE = 2
};

#ifdef __cplusplus
extern "C" {
#endif

struct TargetTaskEntryTy;
typedef struct TargetTaskEntryTy TargetTaskEntryTy;

struct OffloadEntryTy;

struct OffloadingDataEntryTy;

extern TargetTaskEntryTy* CreateTargetTaskEntryTy(
        void *p_tgt_entry_ptr, 
        void **p_tgt_args, 
        ptrdiff_t *p_tgt_offsets, 
        int64_t *p_tgt_arg_types, 
        int32_t p_arg_num);

void chameleon_set_img_idx_offset(TargetTaskEntryTy *task, int32_t img_idx, ptrdiff_t entry_image_offset);

// ================================================================================
// External functions (that can be called from source code or libomptarget)
// ================================================================================
int32_t chameleon_init();

int32_t chameleon_set_image_base_address(int idx_image, intptr_t base_address);

int32_t chameleon_finalize();

int32_t chameleon_distributed_taskwait(int nowait);

int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size);

void chameleon_free_data(void *tgt_ptr);

void chameleon_incr_mem_alloc(int64_t size);

int32_t chameleon_add_task(TargetTaskEntryTy *task);

TargetTaskEntryTy* chameleon_pop_task();

int32_t chameleon_get_last_local_task_id_added();

int32_t chameleon_local_task_has_finished(int32_t task_id);

int32_t wake_up_comm_threads();

int32_t put_comm_threads_to_sleep();

int32_t chameleon_taskyield();

#ifdef __cplusplus
}
#endif

#endif
