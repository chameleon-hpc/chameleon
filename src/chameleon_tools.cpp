#include "cham_statistics.h"
#include "chameleon_common.h"
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

__thread int32_t __ch_thread_data_initialized = 0;
cham_t_data_t __ch_rank_data;
#if CHAMELEON_TOOL_USE_MAP
std::map<int32_t, cham_t_data_t*> __ch_thread_data;
std::mutex __mtx_ch_thread_data;
#else
// another version with fixed array using omp_get_max_threads
cham_t_data_t * __ch_thread_data;
#endif

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
        printf("CHAMELEON_TOOL_LIBRARIES = %s\n", tool_libs);
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

    DBP("cham_t_init: CHAMELEN_TOOL = %s\n", cham_t_env_var);
    
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
        if(cham_t_status.enabled) {
            __ch_rank_data.value = chameleon_comm_rank;
#if !CHAMELEON_TOOL_USE_MAP
            RELP("Initializing __ch_thread_data with %d entries\n", omp_get_max_threads());
            __ch_thread_data = (cham_t_data_t*) malloc(omp_get_max_threads()*sizeof(cham_t_data_t));
#endif
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
static cham_t_set_result_t cham_t_set_callback(cham_t_callbacks_t which, cham_t_callback_t callback) {
    int tmp_val = (int) which;
    // printf("Setting callback %d\n", tmp_val);
    switch(tmp_val) {
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
        default:
            fprintf(stderr, "ERROR: Unable to set callback for specifier %d\n", which);
            return cham_t_set_error;
    }
    return cham_t_set_always;
}

static int cham_t_get_callback(cham_t_callbacks_t which, cham_t_callback_t *callback) {
    // TODO: implement
    // switch(which)
    return 0;
}

cham_t_data_t * cham_t_get_thread_data(void) {
// #if CHAM_STATS_RECORD
//     double cur_time = omp_get_wtime();
// #endif
    cham_t_data_t * thr_data = NULL;
    int32_t cur_gtid = __ch_get_gtid();
#if CHAMELEON_TOOL_USE_MAP
    if(__ch_thread_data_initialized) {
        // find item in map
        std::map<int32_t, cham_t_data_t*>::iterator it;
        __mtx_ch_thread_data.lock();
        it = __ch_thread_data.find(cur_gtid);
        if (it != __ch_thread_data.end()) {
            thr_data = it->second;
        }
        __mtx_ch_thread_data.unlock();
    } else {
        // create new item and save in map
        thr_data = (cham_t_data_t*) malloc(sizeof(cham_t_data_t));
        // thr_data->value = syscall(SYS_gettid);
        __mtx_ch_thread_data.lock();
        __ch_thread_data[cur_gtid] = thr_data;
        __mtx_ch_thread_data.unlock();
        __ch_thread_data_initialized = 1;
    }
// #if CHAM_STATS_RECORD
//     cur_time = omp_get_wtime()-cur_time;
//     atomic_add_dbl(_time_tool_get_thread_data_sum, cur_time);
//     _time_tool_get_thread_data_count++;
// #endif
    return thr_data;
#else
    // if(!__ch_thread_data_initialized) {
    //     __ch_thread_data[cur_gtid].value = syscall(SYS_gettid);
    //     __ch_thread_data_initialized = 1;
    // }
// #if CHAM_STATS_RECORD
//     cur_time = omp_get_wtime()-cur_time;
//     atomic_add_dbl(_time_tool_get_thread_data_sum, cur_time);
//     _time_tool_get_thread_data_count++;
// #endif
    return &(__ch_thread_data[cur_gtid]);
#endif
}

cham_t_data_t * cham_t_get_rank_data(void) {
    return &(__ch_rank_data);
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
    else
        fprintf(stderr, "ERROR: function lookup for name %s not possible.\n", s);
    return (cham_t_interface_fn_t)0;
}