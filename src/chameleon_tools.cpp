
#include "chameleon_common.h"
#include "chameleon_common.cpp"
#include "chameleon_tools.h"
#include "chameleon_tools_internal.h"

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
// int _ch_t_initialized = 0;
// std::mutex _mtx__ch_t_initialized;

cham_t_callbacks_active_t cham_t_enabled;
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
    cham_t_enabled.enabled = 1;

    const char *cham_t_env_var = getenv("CHAMELEON_TOOL");
    if (!cham_t_env_var || !strcmp(cham_t_env_var, ""))
        cham_t_enabled.enabled = 0;
    else if (!strcmp(cham_t_env_var, "disabled"))
        cham_t_enabled.enabled = 0;
    else if (!strcmp(cham_t_env_var, "0"))
        cham_t_enabled.enabled = 0;

    DBP("cham_t_init: CHAMELEN_TOOL = %s\n", cham_t_env_var);
    
    if(cham_t_enabled.enabled)
    {
        // try to load tool
        cham_t_start_tool_result = cham_t_try_start_tool(CHAMELEON_VERSION);
        if (cham_t_start_tool_result) {
            cham_t_enabled.enabled = !!cham_t_start_tool_result->initialize(cham_t_fn_lookup, &(cham_t_start_tool_result->tool_data));
            if (!cham_t_enabled.enabled) {
                return;
            }
        } else {
            cham_t_enabled.enabled = 0;
        }
    }
    
    DBP("cham_t_init: cham_t_enabled = %d\n", cham_t_enabled.enabled);

    _ch_t_initialized = 1;
    _mtx_ch_t_initialized.unlock();
}

void cham_t_fini() {
    if (cham_t_enabled.enabled && cham_t_start_tool_result) {
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
            cham_t_enabled.cham_t_callback_task_create = (cham_t_callback_task_create_t)callback;
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

static cham_t_interface_fn_t cham_t_fn_lookup(const char *s) {
    if(!strcmp(s, "cham_t_set_callback"))
        return (cham_t_interface_fn_t)cham_t_set_callback;
    else if(!strcmp(s, "cham_t_get_callback"))
        return (cham_t_interface_fn_t)cham_t_get_callback;
    else
        fprintf(stderr, "ERROR: function lookup for name %s not possible.\n", s);
    return (cham_t_interface_fn_t)0;
}