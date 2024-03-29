#include "chameleon_statistics.h"
#include "chameleon_common.h"
#include "commthread.h"
#include "chameleon_tools.h"
#include "chameleon_tools_internal.h"
#include <map>
#include <omp.h>

/*****************************************************************************
 * Forward declarations
 ****************************************************************************/
static cham_t_interface_fn_t cham_t_fn_lookup(const char *s);
typedef cham_t_start_tool_result_t *(*cham_t_start_tool_t)(unsigned int);

/*****************************************************************************
 * Variables
 ****************************************************************************/
int _ch_t_initialized = 0;
std::mutex _mtx_ch_t_initialized;

cham_t_callbacks_active_t cham_t_status;
cham_t_start_tool_result_t *cham_t_start_tool_result = NULL;

/*****************************************************************************
 * Functions
 ****************************************************************************/
cham_t_start_tool_result_t * cham_t_start_tool(unsigned int cham_version) {
    cham_t_start_tool_result_t *ret = NULL;
    // Search next symbol in the current address space. This can happen if the
    // runtime library is linked before the tool. Since glibc 2.2 strong symbols
    // don't override weak symbols that have been found before unless the user
    // sets the environment variable LD_DYNAMIC_WEAK.
    cham_t_start_tool_t next_tool = (cham_t_start_tool_t)dlsym(RTLD_NEXT, "cham_t_start_tool");
    if (next_tool) {
        ret = next_tool(cham_version);
    }
    return ret;
}

static cham_t_start_tool_result_t * cham_t_try_start_tool(unsigned int cham_version) {
    cham_t_start_tool_result_t *ret = NULL;
    cham_t_start_tool_t start_tool = NULL;

    const char sep = ':';

    ret = cham_t_start_tool(cham_version);

    if (ret)
        return ret;

    // Try tool-libraries-var ICV
    const char *tool_libs = getenv("CHAMELEON_TOOL_LIBRARIES");
    if (tool_libs) {
        RELP("CHAMELEON_TOOL_LIBRARIES = %s\n", tool_libs);
        std::string str(tool_libs);
        std::vector<std::string> libs;
        split_string(str, libs, sep);

        for (auto &cur_lib : libs) {
            void *h = dlopen(cur_lib.c_str(), RTLD_LAZY);
            if (h) {
                start_tool = (cham_t_start_tool_t)dlsym(h, "cham_t_start_tool");
                if (start_tool && (ret = (*start_tool)(cham_version)))
                    break;
                else
                    dlclose(h);
            }
        }
    }
    return ret;
}

void cham_t_init() {
    if(_ch_t_initialized)
        return;
    
    _mtx_ch_t_initialized.lock();
    // need to check again
    if(_ch_t_initialized) {
        _mtx_ch_t_initialized.unlock();
        return;
    }

    // default: try to initilize tool, but possibility to disable it
    cham_t_status.enabled = 1;

    const char *cham_t_env_var = getenv("CHAMELEON_TOOL");
    if (!cham_t_env_var || !strcmp(cham_t_env_var, ""))
        cham_t_status.enabled = 1; // default: tool support enabled
    else if (!strcmp(cham_t_env_var, "disabled"))
        cham_t_status.enabled = 0;
    else if (!strcmp(cham_t_env_var, "0"))
        cham_t_status.enabled = 0;

    DBP("cham_t_init: CHAMELEON_TOOL = %s\n", cham_t_env_var);
    
    if(cham_t_status.enabled)
    {
        // try to load tool
        cham_t_start_tool_result = cham_t_try_start_tool(CHAMELEON_VERSION);
        if (cham_t_start_tool_result) {
            cham_t_status.enabled = !!cham_t_start_tool_result->initialize(cham_t_fn_lookup, &(cham_t_start_tool_result->tool_data));
            if (!cham_t_status.enabled) {
                return;
            }
        } else {
            cham_t_status.enabled = 0;
        }
    }
    
    DBP("cham_t_init: cham_t_status = %d\n", cham_t_status.enabled);

    _ch_t_initialized = 1;
    _mtx_ch_t_initialized.unlock();
}

void cham_t_fini() {
    if (cham_t_status.enabled && cham_t_start_tool_result) {
        DBP("cham_t_fini: enter\n");
        cham_t_start_tool_result->finalize(&(cham_t_start_tool_result->tool_data));
    }
}

/*****************************************************************************
 * callbacks
 ****************************************************************************/
static cham_t_set_result_t cham_t_set_callback(cham_t_callback_types_t which, cham_t_callback_t callback) {
    int tmp_val = (int) which;
    // printf("Setting callback %d\n", tmp_val);
    switch(tmp_val) {
        case cham_t_callback_thread_init:
            cham_t_status.cham_t_callback_thread_init = (cham_t_callback_thread_init_t)callback;
            break;
        case cham_t_callback_post_init_serial:
            cham_t_status.cham_t_callback_post_init_serial = (cham_t_callback_post_init_serial_t)callback;
            break;
        case cham_t_callback_thread_finalize:
            cham_t_status.cham_t_callback_thread_finalize = (cham_t_callback_thread_finalize_t)callback;
            break;
        case cham_t_callback_task_create:
            cham_t_status.cham_t_callback_task_create = (cham_t_callback_task_create_t)callback;
            break;
        case cham_t_callback_task_schedule:
            cham_t_status.cham_t_callback_task_schedule = (cham_t_callback_task_schedule_t)callback;
            break;
        case cham_t_callback_encode_task_tool_data:
            cham_t_status.cham_t_callback_encode_task_tool_data = (cham_t_callback_encode_task_tool_data_t)callback;
            break;
        case cham_t_callback_decode_task_tool_data:
            cham_t_status.cham_t_callback_decode_task_tool_data = (cham_t_callback_decode_task_tool_data_t)callback;
            break;
        case cham_t_callback_sync_region:
            cham_t_status.cham_t_callback_sync_region = (cham_t_callback_sync_region_t)callback;
            break;
        case cham_t_callback_determine_local_load:
            cham_t_status.cham_t_callback_determine_local_load = (cham_t_callback_determine_local_load_t)callback;
            break;
        case cham_t_callback_select_num_tasks_to_offload:
            cham_t_status.cham_t_callback_select_num_tasks_to_offload = (cham_t_callback_select_num_tasks_to_offload_t)callback;
            break;
        case cham_t_callback_select_tasks_for_migration:
            cham_t_status.cham_t_callback_select_tasks_for_migration = (cham_t_callback_select_tasks_for_migration_t)callback;
            break;
        case cham_t_callback_select_num_tasks_to_replicate:
            cham_t_status.cham_t_callback_select_num_tasks_to_replicate = (cham_t_callback_select_num_tasks_to_replicate_t)callback;
            break;
        case cham_t_callback_change_freq_for_execution:
            cham_t_status.cham_t_callback_change_freq_for_execution = (cham_t_callback_change_freq_for_execution_t)callback;
            break;
        default:
            fprintf(stderr, "ERROR: Unable to set callback for specifier %d\n", which);
            return cham_t_set_error;
    }
    return cham_t_set_always;
}

static int cham_t_get_callback(cham_t_callback_types_t which, cham_t_callback_t *callback) {
    // TODO: implement, but currently i dont see any reason/use case for that except unit testing
    return 0;
}

cham_t_data_t * cham_t_get_thread_data(void) {
#if CHAMELEON_TOOL_SUPPORT
    int32_t cur_gtid = __ch_get_gtid();
    return &(__thread_data[cur_gtid].thread_tool_data);
#else
    return nullptr;
#endif
}

cham_t_data_t * cham_t_get_rank_data(void) {
#if CHAMELEON_TOOL_SUPPORT
    return &(__rank_data.rank_tool_data);
#else
    return nullptr;
#endif
}

static cham_t_data_t * cham_t_get_task_data(TYPE_TASK_ID task_id) {
#if CHAMELEON_TOOL_SUPPORT
    cham_migratable_task_t* task = _map_overall_tasks.find(task_id);
    if(task)
        return &(task->task_tool_data);
#endif
    return nullptr;
}

cham_t_rank_info_t * cham_t_get_rank_info(void) {
#if CHAMELEON_TOOL_SUPPORT
    return &(__rank_data.rank_tool_info);
#else
    return nullptr;
#endif
}

cham_t_task_param_info_t cham_t_get_task_param_info(cham_migratable_task_t* task) {
    cham_t_task_param_info_t ret;
    ret.arg_sizes       = nullptr;
    ret.arg_types       = nullptr;
    ret.arg_pointers    = nullptr;
    ret.num_args        = -1;

#if CHAMELEON_TOOL_SUPPORT
    if(task) {
        ret.num_args            = task->arg_num;        
        if(ret.num_args > 0) {
            ret.arg_sizes       = &(task->arg_sizes[0]);
            ret.arg_types       = &(task->arg_types[0]);
            ret.arg_pointers    = &(task->arg_hst_pointers[0]);
        }
    }
#endif
    return ret;
}

cham_t_task_param_info_t cham_t_get_task_param_info_by_id(TYPE_TASK_ID task_id) {
    cham_migratable_task_t* task = _map_overall_tasks.find(task_id);
    return cham_t_get_task_param_info(task);
}

cham_t_task_meta_info_t cham_t_get_task_meta_info(cham_migratable_task_t* task) {
    cham_t_task_meta_info_t ret;
    ret.entry_ptr               = -1;
    ret.idx_image               = -1;
    ret.entry_image_offset      = -1;

#if CHAMELEON_TOOL_SUPPORT
    if(task) {
        ret.entry_ptr           = task->tgt_entry_ptr;
        ret.idx_image           = task->idx_image;
        ret.entry_image_offset  = task->entry_image_offset;
    }
#endif
    return ret;
}

cham_t_task_meta_info_t cham_t_get_task_meta_info_by_id(TYPE_TASK_ID task_id) {
    cham_migratable_task_t* task = _map_overall_tasks.find(task_id);
    return cham_t_get_task_meta_info(task);
}

static cham_t_interface_fn_t cham_t_fn_lookup(const char *s) {
    if(!strcmp(s, "cham_t_set_callback"))
        return (cham_t_interface_fn_t)cham_t_set_callback;
    else if(!strcmp(s, "cham_t_get_callback"))
        return (cham_t_interface_fn_t)cham_t_get_callback;
    else if(!strcmp(s, "cham_t_get_thread_data"))
        return (cham_t_interface_fn_t)cham_t_get_thread_data;
    else if(!strcmp(s, "cham_t_get_rank_data"))
        return (cham_t_interface_fn_t)cham_t_get_rank_data;
    else if(!strcmp(s, "cham_t_get_rank_info"))
        return (cham_t_interface_fn_t)cham_t_get_rank_info;
    else if(!strcmp(s, "cham_t_get_task_param_info"))
        return (cham_t_interface_fn_t)cham_t_get_task_param_info;
    else if(!strcmp(s, "cham_t_get_task_param_info_by_id"))
        return (cham_t_interface_fn_t)cham_t_get_task_param_info_by_id;
    else if(!strcmp(s, "cham_t_get_task_meta_info"))
        return (cham_t_interface_fn_t)cham_t_get_task_meta_info;
    else if(!strcmp(s, "cham_t_get_task_meta_info_by_id"))
        return (cham_t_interface_fn_t)cham_t_get_task_meta_info_by_id;
    else if(!strcmp(s, "cham_t_get_task_data"))
        return (cham_t_interface_fn_t)cham_t_get_task_data;
    else
        fprintf(stderr, "ERROR: function lookup for name %s not possible.\n", s);
    return (cham_t_interface_fn_t)0;
}
