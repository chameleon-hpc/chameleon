#ifndef _CHAMELEON_TOOLS_INTERNAL_H_
#define _CHAMELEON_TOOLS_INTERNAL_H_

#include "chameleon_common.h"
#include "chameleon_tools.h"

typedef struct cham_t_callbacks_active_s {
    unsigned int enabled : 1;
    
    // list of callback pointers
    cham_t_callback_thread_init_t                   cham_t_callback_thread_init                 = nullptr;
    cham_t_callback_post_init_serial_t              cham_t_callback_post_init_serial            = nullptr;
    cham_t_callback_thread_finalize_t               cham_t_callback_thread_finalize             = nullptr;
    cham_t_callback_task_create_t                   cham_t_callback_task_create                 = nullptr;
    cham_t_callback_task_schedule_t                 cham_t_callback_task_schedule               = nullptr;
    cham_t_callback_encode_task_tool_data_t         cham_t_callback_encode_task_tool_data       = nullptr;
    cham_t_callback_decode_task_tool_data_t         cham_t_callback_decode_task_tool_data       = nullptr;
    cham_t_callback_sync_region_t                   cham_t_callback_sync_region                 = nullptr;
    cham_t_callback_determine_local_load_t          cham_t_callback_determine_local_load        = nullptr;
    cham_t_callback_select_num_tasks_to_offload_t   cham_t_callback_select_num_tasks_to_offload = nullptr;
    cham_t_callback_select_num_tasks_to_replicate_t cham_t_callback_select_num_tasks_to_replicate = nullptr;
    cham_t_callback_select_tasks_for_migration_t    cham_t_callback_select_tasks_for_migration  = nullptr;
    cham_t_callback_change_freq_for_execution_t     cham_t_callback_change_freq_for_execution   = nullptr;  // change frequency

} cham_t_callbacks_active_t;

extern cham_t_callbacks_active_t cham_t_status;

#ifdef __cplusplus
extern "C" {
#endif

void cham_t_init(void);

void cham_t_fini(void);

cham_t_data_t * cham_t_get_thread_data(void);
cham_t_data_t * cham_t_get_rank_data(void);
static cham_t_data_t * cham_t_get_task_data(TYPE_TASK_ID task_id);

#ifdef __cplusplus
};
#endif

#endif
