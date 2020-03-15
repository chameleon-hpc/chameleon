#ifndef _CHAMELEON_TOOLS_H_
#define _CHAMELEON_TOOLS_H_

#include <stddef.h>
#include <stdint.h>
#include <dlfcn.h>

#include <list>
#include <mutex>
#include <atomic>

#pragma region Enums and Definitions
/*****************************************************************************
 * Enums
 ****************************************************************************/
typedef enum cham_t_callback_types_t {
    cham_t_callback_thread_init                 = 1,
    cham_t_callback_thread_finalize             = 2,
    cham_t_callback_task_create                 = 3,
    cham_t_callback_encode_task_tool_data       = 4,
    cham_t_callback_decode_task_tool_data       = 5,
    cham_t_callback_task_schedule               = 6,
    cham_t_callback_sync_region                 = 7,
    cham_t_callback_determine_local_load        = 8,
    cham_t_callback_select_num_tasks_to_offload = 9,
    cham_t_callback_select_tasks_for_migration  = 10,
    cham_t_callback_select_num_tasks_to_replicate= 11
    // cham_t_callback_implicit_task            = 7,
    // cham_t_callback_target                   = 8,
    // cham_t_callback_target_data_op           = 9,
    // cham_t_callback_target_submit            = 10,
    // cham_t_callback_control_tool             = 11,
    // cham_t_callback_device_initialize        = 12,
    // cham_t_callback_device_finalize          = 13,
    // cham_t_callback_device_load              = 14,
    // cham_t_callback_device_unload            = 15,
    // cham_t_callback_sync_region_wait         = 16,
    // cham_t_callback_mutex_released           = 17,
    // cham_t_callback_dependences              = 18,
    // cham_t_callback_task_dependence          = 19,
    // cham_t_callback_work                     = 20,
    // cham_t_callback_master                   = 21,
    // cham_t_callback_target_map               = 22,
    // cham_t_callback_sync_region              = 23,
    // cham_t_callback_lock_init                = 24,
    // cham_t_callback_lock_destroy             = 25,
    // cham_t_callback_mutex_acquire            = 26,
    // cham_t_callback_mutex_acquired           = 27,
    // cham_t_callback_nest_lock                = 28,
    // cham_t_callback_flush                    = 29,
    // cham_t_callback_cancel                   = 30,
    // cham_t_callback_reduction                = 31,
    // cham_t_callback_dispatch                 = 32
} cham_t_callback_types_t;

typedef enum cham_t_set_result_t {
    cham_t_set_error            = 0,
    cham_t_set_never            = 1,
    cham_t_set_impossible       = 2,
    cham_t_set_sometimes        = 3,
    cham_t_set_sometimes_paired = 4,
    cham_t_set_always           = 5
} cham_t_set_result_t;

typedef enum cham_t_task_schedule_type_t {
    cham_t_task_start           = 1,
    cham_t_task_yield           = 2,
    cham_t_task_end             = 3,
    cham_t_task_cancel          = 4
} cham_t_task_schedule_type_t;

static const char* cham_t_task_schedule_type_t_values[] = {
    NULL,
    "cham_t_task_start",        // 1
    "cham_t_task_yield",        // 2
    "cham_t_task_end",          // 3
    "cham_t_task_cancel"        // 4
};

typedef enum cham_t_task_flag_t {
    cham_t_task_local           = 0x00000001,
    cham_t_task_remote          = 0x00000002,
    cham_t_task_replicated      = 0x00000004
} cham_t_task_flag_t;

static void cham_t_task_flag_t_value(int type, char *buffer) {
  char *progress = buffer;
  if (type & cham_t_task_local)
    progress += sprintf(progress, "cham_t_task_local");
  if (type & cham_t_task_remote)
    progress += sprintf(progress, "cham_t_task_remote");
  if (type & cham_t_task_replicated)
    progress += sprintf(progress, "cham_t_task_replicated");
}

typedef enum cham_t_sync_region_type_t {
    cham_t_sync_region_taskwait  = 1
} cham_t_sync_region_type_t;

static const char* cham_t_sync_region_type_t_values[] = {
    NULL,
    "cham_t_sync_region_taskwait"       // 1
};

typedef enum cham_t_sync_region_status_t {
    cham_t_sync_region_start    = 1,
    cham_t_sync_region_end      = 2
} cham_t_sync_region_status_t;

static const char* cham_t_sync_region_status_t_values[] = {
    NULL,
    "cham_t_sync_region_start",         // 1
    "cham_t_sync_region_end"            // 2
};

/*****************************************************************************
 * General definitions
 ****************************************************************************/
typedef struct cham_t_rank_info_t {
    int32_t comm_rank;
    int32_t comm_size;
} cham_t_rank_info_t;

typedef void (*cham_t_interface_fn_t) (void);

typedef cham_t_interface_fn_t (*cham_t_function_lookup_t) (
    const char *interface_function_name
);

typedef void (*cham_t_callback_t) (void);

// either interprete data type as value or pointer
typedef union cham_t_data_t {
    uint64_t value;
    void *ptr;
} cham_t_data_t;

typedef struct cham_t_migration_tupel_t {
    TYPE_TASK_ID task_id;
    int32_t rank_id;
} cham_t_migration_tupel_t;

static cham_t_migration_tupel_t cham_t_migration_tupel_create(TYPE_TASK_ID task_id, int32_t rank_id) {
    cham_t_migration_tupel_t val;
    val.task_id = task_id;
    val.rank_id = rank_id;
    return val;
}

typedef struct cham_t_replication_info_t {
	int num_tasks, num_replication_ranks;
	int *replication_ranks;
} cham_t_replication_info_t;

static cham_t_replication_info_t cham_t_replication_info_create(int num_tasks, int num_replication_ranks, int *replication_ranks) {
	cham_t_replication_info_t info;
	info.num_tasks = num_tasks;
	info.num_replication_ranks = num_replication_ranks;
	info.replication_ranks = replication_ranks;
	return info;
}

static void free_replication_info(cham_t_replication_info_t *info) {
	free(info->replication_ranks);
	info = NULL;
}

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
 * Data for the tool
 ****************************************************************************/
typedef struct cham_t_task_info_t {
    TYPE_TASK_ID task_id;
    int rank_belong;    // 0
    size_t size_data;   // 1
    double queue_time;  // 2
    double start_time;  // 3
    double end_time;    // 4
    double mig_time;    // 5
    double exe_time;    // 6
    bool migrated;      // 7
} cham_t_task_info_t;

typedef struct cham_t_task_lis_t {
    std::list<cham_t_task_info_t*> task_list;
    std::mutex m;
    std::atomic<size_t> list_size;

    void cham_t_task_list(){
        list_size = 0;
    }

    size_t size() {
        return this->list_size.load();
    }

    bool empty() {
        return this->list_size <= 0;
    }

    void push_back(cham_t_task_info_t* task) {
        this->m.lock();
        this->task_list.push_back(task);
        this->list_size++;
        this->m.unlock();
    }

    void set_start_time(TYPE_TASK_ID task_id, double s_time){
        this->m.lock();
        for (std::list<cham_t_task_info_t*>::iterator it=this->task_list.begin(); it!=this->task_list.end(); ++it){
            if ((*it)->task_id == task_id)
                (*it)->start_time = s_time;
        }
        this->m.unlock();
    }

    void set_migrated_time(TYPE_TASK_ID task_id, double m_time){
        this->m.lock();
        for (std::list<cham_t_task_info_t*>::iterator it=this->task_list.begin(); it!=this->task_list.end(); ++it){
            if ((*it)->task_id == task_id){
                (*it)->migrated = true;
                (*it)->mig_time = m_time;
            }
        }
    }

} cham_t_task_lis_t;

/*****************************************************************************
 * Getter / Setter
 ****************************************************************************/
typedef cham_t_set_result_t (*cham_t_set_callback_t) (
    cham_t_callback_types_t event,
    cham_t_callback_t callback
);

typedef int (*cham_t_get_callback_t) (
    cham_t_callback_types_t event,
    cham_t_callback_t *callback
);

typedef cham_t_data_t *(*cham_t_get_thread_data_t) (void);
typedef cham_t_data_t *(*cham_t_get_rank_data_t) (void);
typedef cham_t_data_t *(*cham_t_get_task_data_t) (TYPE_TASK_ID);

typedef cham_t_rank_info_t *(*cham_t_get_rank_info_t) (void);

/*****************************************************************************
 * List of callbacks
 ****************************************************************************/
typedef void (*cham_t_callback_thread_init_t) (
    cham_t_data_t *thread_data
);

typedef void (*cham_t_callback_thread_finalize_t) (
    cham_t_data_t *thread_data
);

typedef void (*cham_t_callback_task_create_t) (
    cham_migratable_task_t * task,                   // opaque data type for internal task
    cham_t_data_t *task_data,
    const void *codeptr_ra
);

typedef void (*cham_t_callback_task_schedule_t) (
    cham_migratable_task_t * task,                   // opaque data type for internal task
    cham_t_task_flag_t task_flag,
    cham_t_data_t *task_data,
    cham_t_task_schedule_type_t schedule_type,
    cham_migratable_task_t * prior_task,             // opaque data type for internal task
    cham_t_task_flag_t prior_task_flag,
    cham_t_data_t *prior_task_data
);

// Encode custom task tool data (if any has been set) for a task that will be migrated to remote rank.
// Ensures that this data is also send to remote rank and available in tool calls.
// Note: Only necessary when task specific data is required
// Note: Only works in combination with cham_t_callback_decode_task_tool_data_t
typedef void *(*cham_t_callback_encode_task_tool_data_t) (
    cham_migratable_task_t * task,                   // opaque data type for internal task
    cham_t_data_t *task_data,
    int32_t *size
);

// Decode custom task tool data (if any has been set) for a task that has been migrated to remote rank.
// Restore data in corresponding task_data struct
// Note: Only necessary when task specific data is required
// Note: Only works in combination with cham_t_callback_encode_task_tool_data_t
typedef void (*cham_t_callback_decode_task_tool_data_t) (
    cham_migratable_task_t * task,                   // opaque data type for internal task
    cham_t_data_t *task_data,
    void *buffer,
    int32_t size
);

typedef void (*cham_t_callback_sync_region_t) (
    cham_t_sync_region_type_t sync_region_type,
    cham_t_sync_region_status_t sync_region_status,
    cham_t_data_t *thread_data,
    const void *codeptr_ra
);

typedef int32_t (*cham_t_callback_determine_local_load_t) (
    TYPE_TASK_ID* task_ids_local,
    int32_t num_tasks_local,
    TYPE_TASK_ID* task_ids_local_rep,
    int32_t num_tasks_local_rep,
    TYPE_TASK_ID* task_ids_stolen,
    int32_t num_tasks_stolen,
    TYPE_TASK_ID* task_ids_stolen_rep,
    int32_t num_tasks_stolen_rep
);

// information about current rank and number of ranks can be achived with cham_t_get_rank_info_t
typedef void (*cham_t_callback_select_num_tasks_to_offload_t) (
    int32_t* num_tasks_to_offload_per_rank,
    const int32_t* load_info_per_rank,
    int32_t num_tasks_local,
    int32_t num_tasks_stolen
);

typedef cham_t_replication_info_t* (*cham_t_callback_select_num_tasks_to_replicate_t) (
    const int32_t* load_info_per_rank,
    int32_t num_tasks_local,
    int32_t *num_replication_infos
);

// information about current rank and number of ranks can be achived with cham_t_get_rank_info_t
// task annotations can be queried with TODO
typedef cham_t_migration_tupel_t* (*cham_t_callback_select_tasks_for_migration_t) (
    const int32_t* load_info_per_rank,
    TYPE_TASK_ID* task_ids_local,
    int32_t num_tasks_local,
    int32_t num_tasks_stolen,
    int32_t* num_tuples
);

#pragma endregion

#endif
