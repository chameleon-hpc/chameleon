#ifndef _CHAMELEON_TOOLS_H_
#define _CHAMELEON_TOOLS_H_

// #include "chameleon_common.h"
#include <stddef.h>
#include <stdint.h>
#include <dlfcn.h>

#pragma region Enums and Definitions
/*****************************************************************************
 * Enums
 ****************************************************************************/
typedef enum cham_t_callbacks_t {
    cham_t_callback_thread_begin             = 1,
    cham_t_callback_thread_end               = 2,
    cham_t_callback_parallel_end             = 4,
    cham_t_callback_task_create              = 5,
    cham_t_callback_task_schedule            = 6,
    cham_t_callback_implicit_task            = 7,
    cham_t_callback_target                   = 8,
    cham_t_callback_target_data_op           = 9,
    cham_t_callback_target_submit            = 10,
    cham_t_callback_control_tool             = 11,
    cham_t_callback_device_initialize        = 12,
    cham_t_callback_device_finalize          = 13,
    cham_t_callback_device_load              = 14,
    cham_t_callback_device_unload            = 15,
    cham_t_callback_sync_region_wait         = 16,
    cham_t_callback_mutex_released           = 17,
    cham_t_callback_dependences              = 18,
    cham_t_callback_task_dependence          = 19,
    cham_t_callback_work                     = 20,
    cham_t_callback_master                   = 21,
    cham_t_callback_target_map               = 22,
    cham_t_callback_sync_region              = 23,
    cham_t_callback_lock_init                = 24,
    cham_t_callback_lock_destroy             = 25,
    cham_t_callback_mutex_acquire            = 26,
    cham_t_callback_mutex_acquired           = 27,
    cham_t_callback_nest_lock                = 28,
    cham_t_callback_flush                    = 29,
    cham_t_callback_cancel                   = 30,
    cham_t_callback_reduction                = 31,
    cham_t_callback_dispatch                 = 32
} cham_t_callbacks_t;

typedef enum cham_t_set_result_t {
    cham_t_set_error            = 0,
    cham_t_set_never            = 1,
    cham_t_set_impossible       = 2,
    cham_t_set_sometimes        = 3,
    cham_t_set_sometimes_paired = 4,
    cham_t_set_always           = 5
} cham_t_set_result_t;

/*****************************************************************************
 * General definitions
 ****************************************************************************/
typedef void (*cham_t_interface_fn_t) (void);

typedef cham_t_interface_fn_t (*cham_t_function_lookup_t) (
    const char *interface_function_name
);

typedef void (*cham_t_callback_t) (void);

typedef union cham_t_data_t {
    uint64_t value;
    void *ptr;
} cham_t_data_t;

/*****************************************************************************
 * Init / Finalize / Start Tool
 ****************************************************************************/
typedef void (*cham_t_finalize_t) (
    cham_t_data_t *tool_data
);

typedef int (*cham_t_initialize_t) (
    cham_t_function_lookup_t lookup,
    cham_t_data_t *tool_data
);

typedef struct cham_t_start_tool_result_t {
    cham_t_initialize_t initialize;
    cham_t_finalize_t finalize;
    cham_t_data_t tool_data;
} cham_t_start_tool_result_t;

/*****************************************************************************
 * Getter / Setter
 ****************************************************************************/
typedef cham_t_set_result_t (*cham_t_set_callback_t) (
    cham_t_callbacks_t event,
    cham_t_callback_t callback
);

typedef int (*cham_t_get_callback_t) (
    cham_t_callbacks_t event,
    cham_t_callback_t *callback
);

/*****************************************************************************
 * List of callbacks
 ****************************************************************************/
typedef void (*cham_t_callback_thread_end_t) (
    cham_t_data_t *thread_data
);

typedef void (*cham_t_callback_task_create_t) (
    TargetTaskEntryTy * task,
    cham_t_data_t *rank_data,
    cham_t_data_t *thread_data,
    cham_t_data_t *task_data    
);

#pragma endregion

#endif