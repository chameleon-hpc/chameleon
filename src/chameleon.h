// Chameleon Header
#ifndef _CHAMELEON_H_
#define _CHAMELEON_H_

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <omp.h>
#include <sched.h>
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

#ifndef TYPE_TASK_ID
#define TYPE_TASK_ID int
#endif

#define CHAMELEON_VERSION 10

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

typedef enum chameleon_device_ids_t {
  CHAMELEON_HOST    = 1001,
  CHAMELEON_MPI     = 1002,
} chameleon_device_ids_t;

typedef enum chameleon_result_types_t {
    CHAM_SUCCESS = 0,
    CHAM_FAILURE = 1,

    CHAM_LOCAL_TASK_NONE = 2,
    CHAM_LOCAL_TASK_SUCCESS = 3,
    CHAM_LOCAL_TASK_FAILURE = 4,

    CHAM_REMOTE_TASK_NONE = 5,
    CHAM_REMOTE_TASK_SUCCESS = 6,
    CHAM_REMOTE_TASK_FAILURE = 7,  

    CHAM_REPLICATED_TASK_NONE = 8,
    CHAM_REPLICATED_TASK_SUCCESS = 9,
    CHAM_REPLICATED_TASK_ALREADY_AVAILABLE = 10,
    CHAM_REPLICATED_TASK_FAILURE = 11 
} chameleon_result_types_t;

typedef struct chameleon_map_data_entry_t {
    void *valptr;
    size_t size;    // size of parameter in bytes
    int type;       // int representing bitwise combination of chameleon_tgt_map_type values
} chameleon_map_data_entry_t;

static chameleon_map_data_entry_t chameleon_map_data_entry_create(void* arg_ptr, size_t arg_size, int arg_type) {
    chameleon_map_data_entry_t entry;
    entry.valptr    = arg_ptr;
    entry.size      = arg_size;
    entry.type      = arg_type;
    return entry;
}

typedef void (*chameleon_external_callback_t)(void* func_param);

#ifdef __cplusplus
extern "C" {
#endif

// opaque object representing annotation container
struct chameleon_annotations_t;
typedef struct chameleon_annotations_t chameleon_annotations_t;

// opaque object representing migratable task
struct cham_migratable_task_t;
typedef struct cham_migratable_task_t cham_migratable_task_t;

// ================================================================================
// Handling annotations
// ================================================================================
chameleon_annotations_t* chameleon_create_annotation_container();

int chameleon_set_annotation_int(chameleon_annotations_t* ann, char *key, int value);
int chameleon_set_annotation_int64(chameleon_annotations_t* ann, char *key, int64_t value);
int chameleon_set_annotation_double(chameleon_annotations_t* ann, char *key, double value);
int chameleon_set_annotation_float(chameleon_annotations_t* ann, char *key, float value);
int chameleon_set_annotation_string(chameleon_annotations_t* ann, char *key, char *value);

int chameleon_get_annotation_int(chameleon_annotations_t* ann, char *key, int* val);
int chameleon_get_annotation_int64(chameleon_annotations_t* ann, char *key, int64_t* val);
int chameleon_get_annotation_double(chameleon_annotations_t* ann, char *key, double* val);
int chameleon_get_annotation_float(chameleon_annotations_t* ann, char *key, float* val);
int chameleon_get_annotation_string(chameleon_annotations_t* ann, char *key, char** val);

// void* chameleon_create_annotation_container_fortran();
// int chameleon_set_annotation_int_fortran(void* ann, int value);
// int chameleon_get_annotation_int_fortran(void* ann);

chameleon_annotations_t* chameleon_get_task_annotations(TYPE_TASK_ID task_id);
chameleon_annotations_t* chameleon_get_task_annotations_opaque(cham_migratable_task_t* task);

void chameleon_set_task_annotations(cham_migratable_task_t* task, chameleon_annotations_t* ann);

// ================================================================================
// Handling replication
// ================================================================================
void chameleon_set_task_replication_info(cham_migratable_task_t* task, int num_replication_ranks, int *replication_ranks);

// ================================================================================
// Init / Finalize / Taskwait / Utils
// ================================================================================
int32_t chameleon_init();

int32_t chameleon_thread_init();

int32_t chameleon_set_image_base_address(int idx_image, intptr_t base_address);

int32_t chameleon_finalize();

int32_t chameleon_thread_finalize();

int32_t chameleon_distributed_taskwait(int nowait);

int32_t chameleon_wake_up_comm_threads();

int32_t chameleon_taskyield();

void chameleon_print(int print_prefix, const char *prefix, int rank, ... );

int32_t chameleon_determine_base_addresses(void * main_ptr);

void chameleon_set_proc_cpuset(cpu_set_t mask);

void chameleon_set_tracing_enabled(int enabled);

// ================================================================================
// Functions for data and task handling - used by libomptarget approach
// ================================================================================
int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size);

void chameleon_free_data(void *tgt_ptr);

void chameleon_incr_mem_alloc(int64_t size);

cham_migratable_task_t* create_migratable_task(
        void *p_tgt_entry_ptr, 
        void **p_tgt_args, 
        ptrdiff_t *p_tgt_offsets, 
        int64_t *p_tgt_arg_types, 
        int32_t p_arg_num);

void chameleon_set_img_idx_offset(cham_migratable_task_t *task, int32_t img_idx, ptrdiff_t entry_image_offset);

// ================================================================================
// Functions related to tasks
// ================================================================================
cham_migratable_task_t* chameleon_create_task(void * entry_point, int num_args, chameleon_map_data_entry_t* args);

void chameleon_set_callback_task_finish(cham_migratable_task_t *task, chameleon_external_callback_t func_ptr, void *func_param);

// not 100% sure whether we still need fortran specific functions
void* chameleon_create_task_fortran(void * entry_point, int num_args, void* args);

int32_t chameleon_add_task(cham_migratable_task_t *task);

// not 100% sure whether we still need fortran specific functions
int32_t chameleon_add_task_fortran(void *task);

TYPE_TASK_ID chameleon_get_last_local_task_id_added();

int32_t chameleon_local_task_has_finished(TYPE_TASK_ID task_id);

TYPE_TASK_ID chameleon_get_task_id(cham_migratable_task_t *task);

#ifdef __cplusplus
}
#endif

#endif
