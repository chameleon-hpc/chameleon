#include <ffi.h>
#include <mpi.h>
#include <stdexcept>
#include <algorithm>
#include <cassert>
#include <dlfcn.h>
#include <link.h>

#include "chameleon.h"
#include "chameleon_common.h"
#include "commthread.h"
#include "chameleon_statistics.h"
#include "chameleon_version.h"
#include "chameleon_tools.h"
#include "chameleon_tools_internal.h"

#ifdef TRACE
#include "VT.h"
#endif

#ifndef DEADLOCK_WARNING_TIMEOUT
#define DEADLOCK_WARNING_TIMEOUT 20
#endif

#ifndef MAX_ATTEMPS_FOR_STANDARD_OPENMP_TASK
#define MAX_ATTEMPS_FOR_STANDARD_OPENMP_TASK 3
#endif

#pragma region Variables
// ================================================================================
// Variables
// ================================================================================
std::mutex _mtx_relp;
#ifdef CHAM_DEBUG
std::atomic<long> mem_allocated;
#endif
// flag that tells whether library has already been initialized
std::mutex _mtx_ch_is_initialized;
std::atomic<bool> _ch_is_initialized(false);
// atomic counter for task ids
std::atomic<TYPE_TASK_ID> _task_id_counter(0);

// id of last task that has been created by thread.
// this is thread local storage
__thread TYPE_TASK_ID __last_task_id_added = -1;

// list with data that has been mapped in map clauses
std::mutex _mtx_data_entry;
std::unordered_map<void*, migratable_data_entry_t*> _data_entries;

// list that holds task ids (created at the current rank) that are not finsihed yet
thread_safe_list_t<TYPE_TASK_ID> _unfinished_locally_created_tasks;

// variables to indicate when it is save to break out of taskwait
std::atomic<int> _flag_dtw_active(0);
std::atomic<int> _num_threads_finished_dtw(0);

// lock used to ensure that currently only a single thread is doing communication progression
std::mutex _mtx_comm_progression;

// total tasks created per rank
int32_t _total_created_tasks_per_rank = 0;

#pragma endregion Variables

// void char_p_f2c(const char* fstr, int len, char** cstr)
// {
//   const char* end;
//   int i;
//   /* Leading and trailing blanks are discarded. */
//   end = fstr + len - 1;
//   for (i = 0; (i < len) && (' ' == *fstr); ++i, ++fstr) {
//     continue;
//   }
//   if (i >= len) {
//     len = 0;
//   } else {
//     for (; (end > fstr) && (' ' == *end); --end) {
//       continue;
//     }
//     len = end - fstr + 1;
//   }
//   /* Allocate space for the C string, if necessary. */
//   if (*cstr == NULL) {
//     if ((*cstr = (char*) malloc(len + 1)) == NULL) {
//       return;
//     }
//   }
//   /* Copy F77 string into C string and NULL terminate it. */
//   if (len > 0) {
//     strncpy(*cstr, fstr, len);
//   }
//   (*cstr)[len] = '\0';
// }

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Forward Declarations
// ================================================================================
// Forward declaration of internal functions (just called inside shared library)
// ================================================================================
int32_t lookup_hst_pointers(cham_migratable_task_t *task);
int32_t execute_target_task(cham_migratable_task_t *task);
int32_t process_local_task();
int32_t process_remote_task();
int32_t process_replicated_local_task();
int32_t process_replicated_migrated_task();
int32_t process_replicated_remote_task();
#pragma endregion Forward Declarations

#pragma region Annotations / Replication
chameleon_annotations_t* chameleon_create_annotation_container() {
    chameleon_annotations_t* container = new chameleon_annotations_t();
    return container;
}

// void* chameleon_create_annotation_container_fortran() {
//     chameleon_annotations_t* container = new chameleon_annotations_t();
//     return (void*)container;
// }

// int chameleon_set_annotation_int_fortran(void* ann, int value) {
//     return chameleon_set_annotation_int((chameleon_annotations_t*)ann, (char*)"num_cells", value);
// }

// int chameleon_get_annotation_int_fortran(void* ann) {
//     int res;
//     int found = chameleon_get_annotation_int((chameleon_annotations_t*) ann, (char*)"num_cells", &res);
//     return found ? res : -1;
// }

int chameleon_set_annotation_int(chameleon_annotations_t* ann, char *key, int value) {
    cham_annotation_value_t val;
    val.val_int32 = value;
    ann->anno.insert(std::make_pair(std::string(key), cham_annotation_entry_value(cham_annotation_int, val)));
    return CHAM_SUCCESS;
}

int chameleon_set_annotation_int64(chameleon_annotations_t* ann, char *key, int64_t value) {
    cham_annotation_value_t val;
    val.val_int64 = value;
    ann->anno.insert(std::make_pair(std::string(key), cham_annotation_entry_value(cham_annotation_int64, val)));
    return CHAM_SUCCESS;
}

int chameleon_set_annotation_double(chameleon_annotations_t* ann, char *key, double value) {
    cham_annotation_value_t val;
    val.val_double = value;
    ann->anno.insert(std::make_pair(std::string(key), cham_annotation_entry_value(cham_annotation_double, val)));
    return CHAM_SUCCESS;
}

int chameleon_set_annotation_float(chameleon_annotations_t* ann, char *key, float value) {
    cham_annotation_value_t val;
    val.val_float = value;
    ann->anno.insert(std::make_pair(std::string(key), cham_annotation_entry_value(cham_annotation_float, val)));
    return CHAM_SUCCESS;
}

int chameleon_set_annotation_string(chameleon_annotations_t* ann, char *key, char *value) {
    cham_annotation_value_t val;
    val.val_ptr = (void*)value;
    ann->anno.insert(std::make_pair(std::string(key), cham_annotation_entry_string(cham_annotation_string, strlen(value), val)));
    return CHAM_SUCCESS;
}

int get_annotation_general(chameleon_annotations_t* ann, char* key, cham_annotation_value_t* val) {
    std::unordered_map<std::string,cham_annotation_entry_t>::const_iterator got = ann->anno.find(std::string(key));
    bool match = got != ann->anno.end();
    if(match) {
        *val = got->second.value;
        return 1;
    } else {
        return 0;
    }
}

int chameleon_get_annotation_int(chameleon_annotations_t* ann, char *key, int* val) {
    cham_annotation_value_t tmp;
    int found = get_annotation_general(ann, key, &tmp);
    if(found)
        *val = tmp.val_int32;
    return found;
}

int chameleon_get_annotation_int64(chameleon_annotations_t* ann, char *key, int64_t* val) {
    cham_annotation_value_t tmp;
    int found = get_annotation_general(ann, key, &tmp);
    if(found)
        *val = tmp.val_int64;
    return found;
}

int chameleon_get_annotation_double(chameleon_annotations_t* ann, char *key, double* val) {
    cham_annotation_value_t tmp;
    int found = get_annotation_general(ann, key, &tmp);
    if(found)
        *val = tmp.val_double;
    return found;
}

int chameleon_get_annotation_float(chameleon_annotations_t* ann, char *key, float* val) {
    cham_annotation_value_t tmp;
    int found = get_annotation_general(ann, key, &tmp);
    if(found)
        *val = tmp.val_float;
    return found;
}

int chameleon_get_annotation_string(chameleon_annotations_t* ann, char *key, char** val) {
    cham_annotation_value_t tmp;
    int found = get_annotation_general(ann, key, &tmp);
    if(found)
        *val = (char*)tmp.val_ptr;
        // strcpy(val, (char*)tmp.val_ptr);
    return found;
}

int chameleon_get_annotation_ptr(chameleon_annotations_t* ann, char *key, void** val) {
    cham_annotation_value_t tmp;
    int found = get_annotation_general(ann, key, &tmp);
    if(found)
        *val = tmp.val_ptr;
    return found;
}

chameleon_annotations_t* chameleon_get_task_annotations(TYPE_TASK_ID task_id) {
    cham_migratable_task_t* task = _map_overall_tasks.find(task_id);
    if(task)
        return task->task_annotations;
    return nullptr;
}

chameleon_annotations_t* chameleon_get_task_annotations_opaque(cham_migratable_task_t* task) {
    if(task)
        return task->task_annotations;
    return nullptr;
}

void chameleon_set_task_annotations(cham_migratable_task_t* task, chameleon_annotations_t* ann) {
    if(task) {
        if (task->task_annotations) {
            // free old annotations first
            task->task_annotations->free_annotations();
            delete task->task_annotations;
        }
        task->task_annotations = ann;
    }
}

void chameleon_set_task_replication_info(cham_migratable_task_t* task, int num_replication_ranks, int *replication_ranks) {
    if(task) {
        if(replication_ranks && num_replication_ranks > 0) {
            // mark as repliacted
            task->is_replicated_task = 1;

            for(int i = 0; i < num_replication_ranks; i++) {
                assert(replication_ranks[i]<chameleon_comm_size);
                task->replication_ranks.push_back( replication_ranks[i] );
            }
        }
    }
}
#pragma endregion Annotations

#pragma region Init / Finalize / Helper
cham_migratable_task_t* create_migratable_task(
        void *p_tgt_entry_ptr, 
        void **p_tgt_args, 
        ptrdiff_t *p_tgt_offsets, 
        int64_t *p_tgt_arg_types, 
        int32_t p_arg_num) {

    cham_migratable_task_t *tmp_task = new cham_migratable_task_t(p_tgt_entry_ptr, p_tgt_args, p_tgt_offsets, p_tgt_arg_types, p_arg_num);
    assert(tmp_task->result_in_progress.load()==false);
    
    return tmp_task;
}

void chameleon_set_img_idx_offset(cham_migratable_task_t *task, int32_t img_idx, ptrdiff_t entry_image_offset) {
    assert(task->result_in_progress.load()==false);
    task->idx_image = img_idx;
    task->entry_image_offset = entry_image_offset;
}

TYPE_TASK_ID chameleon_get_task_id(cham_migratable_task_t *task) {
    return task->task_id;
}

/* 
 * Function verify_initialized
 * Verifies whether library has already been initialized or not.
 * Otherwise it will throw an error. 
 */
inline void verify_initialized() {
    if(!_ch_is_initialized)
        throw std::runtime_error("Chameleon has not been initilized before.");
}

/* 
 * Function chameleon_preinit_ctor
 * Runs before chameleon library is initialized.
 * NOTE: Might not work with libgomp: might not run before libgomp constructor 
 *       is executed which already binds threads if OpenMP affinity policy set.
 */
static void __attribute__((constructor (102))) chameleon_preinit_ctor(void) {
    // fprintf (stderr,"==> chameleon_preinit_ctor\n");
    // first thing: remember original full cpuset of complete process
    sched_getaffinity(getpid(), sizeof(cpu_set_t), &pid_mask);
}

/* 
 * Function __chameleon_set_proc_cpuset
 * Possibility to set process cpuset from outside
 * NOTE: Necessary when using GNU compiler + libgomp
 */
void chameleon_set_proc_cpuset(cpu_set_t mask) {
    pid_mask = mask;
}

/* 
 * Function chameleon_init
 * Initialized chameleon library, communicators and all whats necessary.
 */
int32_t chameleon_init() {
    if(_ch_is_initialized)
        return CHAM_SUCCESS;
    
    _mtx_ch_is_initialized.lock();
    // need to check again
    if(_ch_is_initialized) {
        _mtx_ch_is_initialized.unlock();
        return CHAM_SUCCESS;
    }

    // === DEBUG
    // get_and_print_affinity_mask();
    // === DEBUG

    // check whether MPI is initialized, otherwise do so
    int initialized, err;
    initialized = 0;
    err = MPI_Initialized(&initialized);
    if(!initialized) {
        // MPI_Init(NULL, NULL);
        int provided;
        MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &provided);
    }

    // create separate communicator for chameleon
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm);
    if(err != 0) handle_error_en(err, "MPI_Comm_dup - chameleon_comm");
    MPI_Comm_size(chameleon_comm, &chameleon_comm_size);
    MPI_Comm_rank(chameleon_comm, &chameleon_comm_rank);
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm_mapped);
    if(err != 0) handle_error_en(err, "MPI_Comm_dup - chameleon_comm_mapped");
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm_load);
    if(err != 0) handle_error_en(err, "MPI_Comm_dup - chameleon_comm_load");
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm_cancel);
    if(err != 0) handle_error_en(err, "MPI_Comm_dup - chameleon_comm_cancel");
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm_activate);
    if(err != 0) handle_error_en(err, "MPI_Comm_dup - chameleon_comm_activate");

    MPI_Errhandler_set(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
    MPI_Errhandler_set(chameleon_comm, MPI_ERRORS_RETURN);
    MPI_Errhandler_set(chameleon_comm_mapped, MPI_ERRORS_RETURN);
    MPI_Errhandler_set(chameleon_comm_cancel, MPI_ERRORS_RETURN);
    MPI_Errhandler_set(chameleon_comm_load, MPI_ERRORS_RETURN);

#if PRINT_CONFIG_VALUES
    RELP("chameleon_init: VERSION %s\n", CHAMELEON_VERSION_STRING);
#endif
#ifdef CHAM_DEBUG
    mem_allocated = 0;
#endif

    // load config values that were speicified by environment variables
    load_config_values();
#if PRINT_CONFIG_VALUES
    print_config_values();
#endif

    // initilize thread data here
    __rank_data.comm_rank = chameleon_comm_rank;
    __rank_data.comm_size = chameleon_comm_size;
#if CHAMELEON_TOOL_SUPPORT
    // copy for tool calls
    __rank_data.rank_tool_info.comm_rank = chameleon_comm_rank;
    __rank_data.rank_tool_info.comm_size = chameleon_comm_size;
#endif
    // need +2 for safty measure to cover both communication threads
    __thread_data = (ch_thread_data_t*) malloc((2+omp_get_max_threads())*sizeof(ch_thread_data_t));

#if CHAMELEON_TOOL_SUPPORT
    cham_t_init();
#endif

    _num_replicated_local_tasks_per_victim.resize(chameleon_comm_size);

    _outstanding_jobs_ranks.resize(chameleon_comm_size);
    _load_info_ranks.resize(chameleon_comm_size);
    for(int i = 0; i < chameleon_comm_size; i++) {
        _outstanding_jobs_ranks[i] = 0;
        _load_info_ranks[i] = 0;
        _active_migrations_per_target_rank[i] = 0;
    }
    _outstanding_jobs_sum = 0;
    _task_id_counter = 0;

#ifndef __GNUG__
    // dummy target region to force binary loading, use host offloading for that purpose
    // #pragma omp target device(1001) map(to:stderr) // 1001 = CHAMELEON_HOST
    #pragma omp target device(1001) // 1001 = CHAMELEON_HOST
    {
        printf("chameleon_init - dummy region\n");
    }
#endif

    // initialize communication session data
    chameleon_comm_thread_session_data_t_init();

    #if ENABLE_COMM_THREAD
    #if THREAD_ACTIVATION
    // start comm threads but in sleep mode
    start_communication_threads();
    #endif
    #endif

#if defined(TRACE) && ENABLE_TRACING_FOR_SYNC_CYCLES
    VT_traceoff();
    _tracing_enabled = 0;
#endif
        
    // set flag to ensure that only a single thread is initializing
    _ch_is_initialized = true;
    _mtx_ch_is_initialized.unlock();
    return CHAM_SUCCESS;
}

int32_t chameleon_thread_init() {
    if(!_ch_is_initialized) {
        chameleon_init();
    }
    // make sure basic stuff is initialized
    int32_t gtid = __ch_get_gtid();

#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_thread_init) {
        cham_t_status.cham_t_callback_thread_init(&(__thread_data[gtid].thread_tool_data));
    }
#endif
#if PRINT_AFFINITY_MASKS
    get_and_print_affinity_mask();
#endif
    return CHAM_SUCCESS;
}

/* 
 * Function chameleon_post_init_serial
 * Can run after init and thread init in a serial region
 */
int32_t chameleon_post_init_serial() {
    // make sure basic stuff is initialized
    int32_t gtid = __ch_get_gtid();
#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_post_init_serial) {
        cham_t_status.cham_t_callback_post_init_serial(&(__thread_data[gtid].thread_tool_data));
    }
#endif
    return CHAM_SUCCESS;
}

int32_t chameleon_thread_finalize() {
    // make sure basic stuff is initialized
    int32_t gtid = __ch_get_gtid();
#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_thread_finalize) {
        cham_t_status.cham_t_callback_thread_finalize(&(__thread_data[gtid].thread_tool_data));
    }
#endif
    return CHAM_SUCCESS;
}

/*
 * Function chameleon_incr_mem_alloc
 * Increment counter that measures how much memory is allocated for mapped types
 */
void chameleon_incr_mem_alloc(int64_t size) {
#ifdef CHAM_DEBUG
    mem_allocated += size;
#endif
}

/*
 * Function chameleon_set_image_base_address
 * Sets base address of particular image index.
 * This is necessary to determine the entry point for functions that represent a target construct
 */
int32_t chameleon_set_image_base_address(int idx_image, intptr_t base_address) {
    if(_image_base_addresses.size() < idx_image+1) {
        _image_base_addresses.resize(idx_image+1);
    }
    // set base address for image (last device wins)
    DBP("chameleon_set_image_base_address (enter) Setting base_address: " DPxMOD " for img: %d\n", DPxPTR((void*)base_address), idx_image);
    _image_base_addresses[idx_image] = base_address;
    return CHAM_SUCCESS;
}

/* 
 * Function chameleon_finalize
 * Finalizing and cleaning up chameleon library before the program ends.
 */
int32_t chameleon_finalize() {
    DBP("chameleon_finalize (enter)\n");
    verify_initialized();

    #if ENABLE_COMM_THREAD && THREAD_ACTIVATION
    stop_communication_threads();
    #endif

    // run routine to cleanup all things for work phase
    cleanup_work_phase();

    #if CHAM_STATS_RECORD && CHAM_STATS_PRINT && !CHAM_STATS_PER_SYNC_INTERVAL
    cham_stats_print_stats();
    #endif

#if CHAMELEON_TOOL_SUPPORT
    cham_t_fini();
#endif

    // cleanup
    free(__thread_data);

    DBP("chameleon_finalize (exit)\n");
    return CHAM_SUCCESS;
}

void chameleon_print(int print_prefix, const char *prefix, int rank, ... ) {
    va_list args;
    va_start(args, rank);
    chameleon_dbg_print_help(print_prefix, prefix, rank, args);
    va_end (args);
}

int32_t chameleon_determine_base_addresses(void * main_ptr) {
    //printf("got address %p\n", main_ptr);
    Dl_info info;
    int rc;
    link_map * map = (link_map *)malloc(1000*sizeof(link_map));
    void *start_ptr = (void*)map;
    // struct link_map map;
    rc = dladdr1(main_ptr, &info, (void**)&map, RTLD_DL_LINKMAP);
    // printf("main dli_fname=%s; dli_fbase=%p\n", info.dli_fname, info.dli_fbase);
    chameleon_set_image_base_address(99, (intptr_t)info.dli_fbase);    
    // TODO: keep it simply for now and assume that target function is in main binary
    // If it is necessary to apply different behavior each loaded library has to be covered and analyzed

    // link_map * cur_entry = &map[0];
    // while(cur_entry) {
    //     printf("l_name = %s; l_addr=%ld; l_ld=%p\n", cur_entry->l_name, cur_entry->l_addr, (void*)cur_entry->l_ld);
    //     cur_entry = cur_entry->l_next;
    // }
    free(start_ptr);
    return CHAM_SUCCESS;
}

void chameleon_set_tracing_enabled(int enabled) {
    #ifdef TRACE
    _tracing_enabled = enabled;
    #endif
}
#pragma endregion Init / Finalize / Helper

#pragma region Distributed Taskwait + Taskyield
/*
 * Taskyield will execute either a "offloadable" target task or a regular OpenMP task
 * (Background: Dependecies currently not fully covered. 
 *              Idea: wrap target task with standard OpenMP task with depencencies.
 *              Inside that we need to use a wait and yield approach.
 *              Thus we need to be able to execute either a normal or a target task in this yield.)
 */
int32_t chameleon_taskyield() {
    int32_t res = CHAM_FAILURE;

    // ========== Prio 1: try to execute stolen tasks to overlap computation and communication
    if(!_stolen_remote_tasks.empty()) {
        res = process_remote_task();
        // if task has been executed successfully start from beginning
        if(res == CHAM_REMOTE_TASK_SUCCESS)
            return CHAM_REMOTE_TASK_SUCCESS;
    }
    
    // ========== Prio 2: work on local tasks
    if(!_local_tasks.empty()) {
        res = process_local_task();
        if(res == CHAM_LOCAL_TASK_SUCCESS)
            return CHAM_LOCAL_TASK_SUCCESS;
    }

    // ========== Prio 3: try to execute a standard OpenMP task because that might create new target or Chameleon tasks
    // DBP("Trying to run OpenMP Task\n");
    #pragma omp taskyield

    return CHAM_FAILURE;
}

void dtw_startup() {
    if(_flag_dtw_active)
        return;
    
    _mtx_taskwait.lock();
    // need to check again
    if(_flag_dtw_active) {
        _mtx_taskwait.unlock();
        return;
    }

    #if CHAM_STATS_RECORD && CHAM_STATS_PER_SYNC_INTERVAL
    cham_stats_reset_for_sync_cycle();
    #endif

    //DBP("chameleon_distributed_taskwait - startup, resetting counters\n");
    _num_threads_involved_in_taskwait   = omp_get_num_threads();
    _session_data.num_threads_in_tw     = omp_get_num_threads();
    
    request_manager_send._num_threads_in_dtw    = omp_get_num_threads();
    request_manager_receive._num_threads_in_dtw = omp_get_num_threads();
    request_manager_cancel._num_threads_in_dtw  = omp_get_num_threads();

    _num_threads_idle                   = 0;
    _num_threads_finished_dtw           = 0;

    #if ENABLE_COMM_THREAD //|| ENABLE_TASK_MIGRATION || CHAM_REPLICATION_MODE>0
    // indicating that this has not happend yet for the current sync cycle
    _comm_thread_load_exchange_happend  = 0;
    #else
    // need to set flags to ensure that exit condition is working with deactivated comm thread and migration
    _comm_thread_load_exchange_happend  = 1; 
    _num_ranks_not_completely_idle      = 0;
    #endif /* ENABLE_COMM_THREAD || ENABLE_TASK_MIGRATION */

    _flag_dtw_active = 1;
    _mtx_taskwait.unlock();
}

void dtw_teardown() {
    // last thread should perform the teardown
    int tmp_num = ++_num_threads_finished_dtw;
    //DBP("chameleon_distributed_taskwait - attempt teardown, finished: %d, involved: %d\n", tmp_num, _num_threads_involved_in_taskwait.load());
    if(tmp_num >= _num_threads_involved_in_taskwait.load()) {
        _mtx_taskwait.lock();
        //DBP("chameleon_distributed_taskwait - teardown, resetting counters\n");
        _comm_thread_load_exchange_happend      = 0;
        _num_threads_involved_in_taskwait       = INT_MAX;
        _session_data.num_threads_in_tw         = INT_MAX;
        _num_threads_idle                       = 0;
        _task_id_counter                        = 0;
        _num_ranks_not_completely_idle          = INT_MAX;
        _total_created_tasks_per_rank           = 0;

        #if CHAM_STATS_RECORD && CHAM_STATS_PRINT && CHAM_STATS_PER_SYNC_INTERVAL
        cham_stats_print_stats();
        #endif

        #ifdef CHAM_DEBUG
        DBP("dtw_teardown - still mem_allocated = %ld\n", (long)mem_allocated);
        mem_allocated = 0;
        #endif
        
        _flag_dtw_active = 0;

#if defined(TRACE) && ENABLE_TRACING_FOR_SYNC_CYCLES
        _num_sync_cycle++;
        if(_num_sync_cycle >= ENABLE_TRACE_FROM_SYNC_CYCLE && _num_sync_cycle <= ENABLE_TRACE_TO_SYNC_CYCLE) {
            VT_traceon();
            _tracing_enabled = 1;            
        } else {
            VT_traceoff();
            _tracing_enabled = 0;            
        }
#endif /* ENABLE_TRACING_FOR_SYNC_CYCLES */
        _mtx_taskwait.unlock();
    }

    // currently barrier here to ensure correctness and avoid race condition between startup and teardown
    #pragma omp barrier

#if defined(TRACE) && ENABLE_TRACING_FOR_SYNC_CYCLES
    if(_tracing_enabled) {
        VT_traceon();
    } else {
        VT_traceoff();
    }
#endif /* ENABLE_TRACING_FOR_SYNC_CYCLES */
}

/* 
 * Function chameleon_distributed_taskwait
 * Default distributed taskwait function that will
 *      - start communication thread
 *      - execute local, stolen and replicated tasks
 *      - wait until all global work is done
 * 
 * Also provides the possibility for a nowait if there is a delay caused by stopping the comm threads
 */
int32_t chameleon_distributed_taskwait(int nowait) {
#ifdef TRACE
    static int event_taskwait = -1;
    std::string event_taskwait_name = "taskwait";
    if(event_taskwait == -1) 
        int ierr = VT_funcdef(event_taskwait_name.c_str(), VT_NOCLASS, &event_taskwait);

    VT_BEGIN_CONSTRAINED(event_taskwait);
#endif

    verify_initialized();
    DBP("chameleon_distributed_taskwait (enter)\n");
    _num_threads_active_in_taskwait++;

    #if CHAM_STATS_RECORD
    double time_start_tw = omp_get_wtime();
    #endif /* CHAM_STATS_RECORD */
    
    // startup actions
    dtw_startup();

    #if ENABLE_COMM_THREAD
    #if THREAD_ACTIVATION
    // need to wake threads up if not already done
    chameleon_wake_up_comm_threads();
    #else
    // start communication threads here
    start_communication_threads();
    #endif /* THREAD_ACTIVATION */
    #endif /* ENABLE_COMM_THREAD */

    bool this_thread_idle = false;
    int my_idle_order = -1;
    // at least try to execute this amout of normal OpenMP tasks after rank runs out of offloadable tasks before assuming idle state
    // TODO: I guess a more stable way would be to have an OMP API call to get the number of outstanding tasks (with and without dependencies)
    int this_thread_num_attemps_standard_task = 0;

#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_sync_region) {
        void *codeptr_ra = __builtin_return_address(0);
        int32_t gtid = __ch_get_gtid();
        cham_t_status.cham_t_callback_sync_region(cham_t_sync_region_taskwait, cham_t_sync_region_start, &(__thread_data[gtid].thread_tool_data) , codeptr_ra);
    }
#endif

    int num_threads_in_tw = _num_threads_involved_in_taskwait.load();
    
    #if SHOW_WARNING_DEADLOCK
    double last_time_doing_sth_useful = omp_get_wtime();
    #endif
 
    // as long as there are local tasks run this loop
    int32_t res = CHAM_SUCCESS;

    while(true) {
        res = CHAM_SUCCESS;

        #if SHOW_WARNING_DEADLOCK
        if(omp_get_wtime()-last_time_doing_sth_useful>DEADLOCK_WARNING_TIMEOUT && omp_get_thread_num()==0) {
           fprintf(stderr, "R#%d:\t Deadlock WARNING: idle time above timeout %d s! \n", chameleon_comm_rank, (int)DEADLOCK_WARNING_TIMEOUT);
           fprintf(stderr, "R#%d:\t outstanding jobs local: %d, outstanding jobs remote: %d outstanding jobs replicated local compute: %d outstanding jobs replicated remote: %d \n", chameleon_comm_rank,
                                                                  _num_local_tasks_outstanding.load(),
                                                                  _num_remote_tasks_outstanding.load(),
                                                                  _num_replicated_local_tasks_outstanding_compute.load(),
                                                                  _num_replicated_remote_tasks_outstanding.load());
           request_manager_receive.printRequestInformation();
           request_manager_send.printRequestInformation();
           last_time_doing_sth_useful = omp_get_wtime(); 
        }
        #endif /* SHOW_WARNING_DEADLOCK */

        #if COMMUNICATION_MODE == 1
        if (_mtx_comm_progression.try_lock()) {
        #endif /* COMMUNICATION_MODE */
            //for (int rep = 0; rep < 2; rep++) {
            // need to check whether exit condition already met
            //if (!exit_condition_met(1,0))
            #if COMMUNICATION_MODE > 0
            action_communication_progression(0);
            #endif /* COMMUNICATION_MODE */
            //}
        #if COMMUNICATION_MODE == 1
            _mtx_comm_progression.unlock();
            
        }
        #endif /* COMMUNICATION_MODE */

        #if ENABLE_TASK_MIGRATION
        // ========== Prio 1: try to execute stolen tasks to overlap computation and communication
        if(!_stolen_remote_tasks.empty()) {
     
            #if SHOW_WARNING_DEADLOCK
            last_time_doing_sth_useful = omp_get_wtime();
            #endif

            if(this_thread_idle) {
                // decrement counter again
                my_idle_order = --_num_threads_idle;
                DBP("chameleon_distributed_taskwait - _num_threads_idle decr: %d\n", my_idle_order);
                this_thread_idle = false;
            }
            // this_thread_num_attemps_standard_task = 0;

            res = process_remote_task();

            // if task has been executed successfully start from beginning
            if(res == CHAM_REMOTE_TASK_SUCCESS)
                continue;
        }
        #endif /* ENABLE_TASK_MIGRATION */

        #if !FORCE_MIGRATION
        // ========== Prio 2: work on local tasks
        if(!_local_tasks.empty()) {
   
            #if SHOW_WARNING_DEADLOCK
            last_time_doing_sth_useful = omp_get_wtime();
            #endif
            
            if(this_thread_idle) {
                // decrement counter again
                my_idle_order = --_num_threads_idle;
                DBP("chameleon_distributed_taskwait - _num_threads_idle decr: %d\n", my_idle_order);
                this_thread_idle = false;
            }
            // this_thread_num_attemps_standard_task = 0;

            // try to execute a local task
            res = process_local_task();

            // if task has been executed successfully start from beginning
            if(res == CHAM_LOCAL_TASK_SUCCESS)
                continue;
        }
        #endif /* !FORCE_MIGRATION */

        #if ENABLE_TASK_MIGRATION || CHAM_REPLICATION_MODE>0
        // ========== Prio 3: work on replicated local tasks
        if(!_replicated_local_tasks.empty()) {
            
            #if SHOW_WARNING_DEADLOCK
            last_time_doing_sth_useful = omp_get_wtime();
            #endif
            
            if(this_thread_idle) {
                // decrement counter again
                my_idle_order = --_num_threads_idle;
                DBP("chameleon_distributed_taskwait - _num_threads_idle decr: %d\n", my_idle_order);
                this_thread_idle = false;
            }
            // this_thread_num_attemps_standard_task = 0;

            // try to execute a local task
            res = process_replicated_local_task();

            if(res != CHAM_REPLICATED_TASK_NONE)
                continue;
        }

        #if CHAM_REPLICATION_MODE<4
        // ========== Prio 5: work on replicated migrated tasks
		if(!_replicated_migrated_tasks.empty()) {

			#if SHOW_WARNING_DEADLOCK
			last_time_doing_sth_useful = omp_get_wtime();
			#endif

			if(this_thread_idle) {
				// decrement counter again
				my_idle_order = --_num_threads_idle;
				DBP("chameleon_distributed_taskwait - _num_threads_idle decr: %d\n", my_idle_order);
				this_thread_idle = false;
			}
			// this_thread_num_attemps_standard_task = 0;

			// try to execute a local task
			res = process_replicated_migrated_task();

			if(res != CHAM_REPLICATED_TASK_NONE)
				continue;
		}

        // ========== Prio 6: work on replicated remote tasks
        if(!_replicated_remote_tasks.empty()) {

            #if SHOW_WARNING_DEADLOCK
            last_time_doing_sth_useful = omp_get_wtime();
            #endif

            if(this_thread_idle) {
                // decrement counter again
                my_idle_order = --_num_threads_idle;
                DBP("chameleon_distributed_taskwait - _num_threads_idle decr: %d\n", my_idle_order);
                this_thread_idle = false;
            }
            // this_thread_num_attemps_standard_task = 0;

            // try to execute a local task
            res = process_replicated_remote_task();

            if(res != CHAM_REPLICATED_TASK_NONE)
                continue;
        }
        #endif
        #endif /* ENABLE_TASK_MIGRATION && CHAM_REPLICATION_MODE>0 */

        // ========== Prio 7: work on a regular OpenMP task
        // make sure that we get info about outstanding tasks with dependences
        // to avoid that we miss some tasks
        // if(this_thread_num_attemps_standard_task >= MAX_ATTEMPS_FOR_STANDARD_OPENMP_TASK)
        // {
            // of course only do that once for the thread :)
            if(!this_thread_idle) {
                // increment idle counter again
                my_idle_order = ++_num_threads_idle;
                //DBP("chameleon_distributed_taskwait - _num_threads_idle incre: %d\n", my_idle_order);
                this_thread_idle = true;
            }
        // } else {
        //     // increment attemps that might result in more target tasks
        //     this_thread_num_attemps_standard_task++;
        //     chameleon_taskyield(); // probably necessary to extract dtw core and call that in taskyield as well to ensure proper handling of variable that control exit condition
        //     continue;
        // }

        // ========== Prio 8: check whether to abort procedure
        // only abort if 
        //      - load exchange has happened at least once 
        //      - there are no outstanding jobs left
        //      - all threads entered the taskwait function (on all processes) and are idling

        if(_num_threads_idle >= num_threads_in_tw) {
            if(exit_condition_met(1,0)) {
                break;
            }
        }
    }

    #if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_sync_region) {
        void *codeptr_ra = __builtin_return_address(0);
        int32_t gtid = __ch_get_gtid();
        cham_t_status.cham_t_callback_sync_region(cham_t_sync_region_taskwait, cham_t_sync_region_end, &(__thread_data[gtid].thread_tool_data) , codeptr_ra);
    }
    #endif /* CHAMELEON_TOOL_SUPPORT */
    
    if(!nowait) {
        #pragma omp barrier
    }

    #if CHAM_STATS_RECORD
    double time_tw_elapsed = omp_get_wtime()-time_start_tw;
    atomic_add_dbl(_time_taskwait_sum, time_tw_elapsed);
    _time_taskwait_count++;
    #endif /* CHAM_STATS_RECORD */

    #if ENABLE_COMM_THREAD
    #if THREAD_ACTIVATION
    // put threads to sleep again after sync cycle
    put_comm_threads_to_sleep();
    #else
    // stop threads here - actually the last thread will do that
    stop_communication_threads();
    #endif
    #endif /* ENABLE_COMM_THREAD */

#ifdef TRACE
    VT_END_W_CONSTRAINED(event_taskwait);
#endif

    // tear down actions of distributed taskwait
    dtw_teardown();

    _num_threads_active_in_taskwait--;
    DBP("chameleon_distributed_taskwait (exit)\n");
    return CHAM_SUCCESS;
}
#pragma endregion Distributed Taskwait

#pragma region Fcns for Data and Tasks
/* 
 * Function chameleon_submit_data
 * Submit mapped data that will be used by tasks.
 */
int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size) {
    DBP("chameleon_submit_data (enter) - tgt_ptr: " DPxMOD ", hst_ptr: " DPxMOD ", size: %ld\n", DPxPTR(tgt_ptr), DPxPTR(hst_ptr), size);
    verify_initialized();
#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime()-cur_time;
#endif
    // check list if already in
    // maybe we need to compare all parameters ==> then we need to come up with a splitted maps and a key generated from parameters
    _mtx_data_entry.lock();
    std::unordered_map<void* ,migratable_data_entry_t*>::const_iterator got = _data_entries.find(tgt_ptr);
    bool match = got!=_data_entries.end();
    if(!match) {
        DBP("chameleon_submit_data - new entry for tgt_ptr: " DPxMOD ", hst_ptr: " DPxMOD ", size: %ld\n", DPxPTR(tgt_ptr), DPxPTR(hst_ptr), size);
        migratable_data_entry_t *new_entry = new migratable_data_entry_t(tgt_ptr, hst_ptr, size);
        _data_entries.insert(std::make_pair(tgt_ptr, new_entry));
    }
    _mtx_data_entry.unlock();
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    if(cur_time > 0) {
        atomic_add_dbl(_time_data_submit_sum, cur_time);
        _time_data_submit_count++;
    }
#endif
    DBP("chameleon_submit_data (exit)\n");
    return CHAM_SUCCESS;
}

void chameleon_free_data(void *tgt_ptr) {
#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime()-cur_time;
#endif
    _mtx_data_entry.lock();
    _data_entries.erase(tgt_ptr);
    _mtx_data_entry.unlock();
    free(tgt_ptr);
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    if(cur_time > 0) {
        atomic_add_dbl(_time_data_submit_sum, cur_time);
        _time_data_submit_count++;
    }
#endif
}

cham_migratable_task_t* chameleon_create_task(void * entry_point, int num_args, chameleon_map_data_entry_t* args) {
    // Format of variable input args should always be:
    // 1. void* to data entry
    // 2. number of arguments
    // 3. information about each agument

    std::vector<void *>     arg_hst_pointers(num_args);
    std::vector<int64_t>    arg_sizes(num_args);
    std::vector<int64_t>    arg_types(num_args);
    std::vector<void *>     arg_tgt_pointers(num_args);
    std::vector<ptrdiff_t>  arg_tgt_offsets(num_args);

    for(int i = 0; i < num_args; i++) {
        void * cur_arg          = args[i].valptr;
        int64_t cur_size        = args[i].size;
        int64_t cur_type        = args[i].type;

        arg_hst_pointers[i]     = cur_arg;
        arg_sizes[i]            = cur_size;
        arg_types[i]            = cur_type;
        arg_tgt_pointers[i]     = nullptr;
        arg_tgt_offsets[i]      = 0;
    }

    cham_migratable_task_t *tmp_task = create_migratable_task(entry_point, &arg_tgt_pointers[0], &arg_tgt_offsets[0], &arg_types[0], num_args);
    // calculate offset to base address
    intptr_t base_address   = _image_base_addresses[99];
    ptrdiff_t diff          = (intptr_t) entry_point - base_address;
    chameleon_set_img_idx_offset(tmp_task, 99, diff);

    tmp_task->is_manual_task    = 1;
    tmp_task->arg_hst_pointers  = arg_hst_pointers;
    tmp_task->arg_sizes         = arg_sizes;

    return tmp_task;
}

void* chameleon_create_task_fortran(void * entry_point, int num_args, void* args) {
    chameleon_map_data_entry_t* tmp_args = (chameleon_map_data_entry_t*) args;
    cham_migratable_task_t* tmp_task = chameleon_create_task(entry_point, num_args, tmp_args);
    return (void*)tmp_task;
}

void chameleon_set_callback_task_finish(cham_migratable_task_t *task, chameleon_external_callback_t func_ptr, void *func_param) {
    task->cb_task_finish_func_ptr = func_ptr;
    task->cb_task_finish_func_param = func_param;
}

int32_t chameleon_add_task(cham_migratable_task_t *task) {
    DBP("chameleon_add_task (enter) - task_entry (task_id=%ld): " DPxMOD "(idx:%d;offset:%d) with arg_num: %d\n", task->task_id, DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);
    verify_initialized();
#ifdef TRACE
    static int event_task_create = -1;
    std::string event_name = "task_create";
    if(event_task_create == -1) 
        int ierr = VT_funcdef(event_name.c_str(), VT_NOCLASS, &event_task_create);
    VT_BEGIN_CONSTRAINED(event_task_create);
#endif
    
    // perform lookup only when task has been created with libomptarget
    if(!task->is_manual_task) {
        lookup_hst_pointers(task);
    }

#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_task_create) {
        void *codeptr_ra = __builtin_return_address(0);
        cham_t_status.cham_t_callback_task_create(task, &(task->task_tool_data), codeptr_ra);
    }
#endif
    assert(task->num_outstanding_recvbacks==0);

    _num_local_tasks_outstanding++;
    DBP("chameleon_add_task - increment local outstanding count for task %ld\n", task->task_id);
    
    _local_tasks.push_back(task);

    // update value total tasks created per rank
    _total_created_tasks_per_rank++;

    // set id of last task added
    __last_task_id_added = task->task_id;

    #if CHAMELEON_ENABLE_FINISHED_TASK_TRACKING
    _unfinished_locally_created_tasks.push_back(task->task_id);
    #endif
    _map_overall_tasks.insert(task->task_id, task);
    
#ifdef TRACE
    VT_END_W_CONSTRAINED(event_task_create);
#endif
    return CHAM_SUCCESS;
}

int32_t chameleon_add_task_fortran(void *task) {
    return chameleon_add_task(((cham_migratable_task_t*)task));
}

TYPE_TASK_ID chameleon_get_last_local_task_id_added() {
    return __last_task_id_added;
}

/*
 * Checks whether the corresponding task has already been finished
 */
int32_t chameleon_local_task_has_finished(TYPE_TASK_ID task_id) {
    #if CHAMELEON_ENABLE_FINISHED_TASK_TRACKING
    bool found = _unfinished_locally_created_tasks.find(task_id);
    return found ? 0 : 1;
    #else
    return 0;
    #endif
}

#pragma endregion Fcns for Data and Tasks

#pragma region Fcns for Lookups and Execution
int32_t lookup_hst_pointers(cham_migratable_task_t *task) {
    DBP("lookup_hst_pointers (enter) - task_entry (task_id=%ld): " DPxMOD "\n", task->task_id, DPxPTR(task->tgt_entry_ptr));
    for(int i = 0; i < task->arg_num; i++) {
        // get type and pointer
        int64_t tmp_type    = task->arg_types[i];
        void * tmp_tgt_ptr  = task->arg_tgt_pointers[i];
        int is_lit          = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        int is_from         = tmp_type & CHAM_OMP_TGT_MAPTYPE_FROM;

        if(tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL) {
            // pointer represents numerical value that is implicitly mapped
            task->arg_hst_pointers[i] = tmp_tgt_ptr;
            task->arg_sizes[i] = sizeof(void *);
            print_arg_info_w_tgt("lookup_hst_pointers", task, i);
        } else {
#if CHAM_STATS_RECORD
            double cur_time = omp_get_wtime()-cur_time;
#endif
            // here we need to perform a pointer mapping to host pointer
            // because target pointers have already been freed and deleted
            int found = 0;
            int count = 0;
            while(!found && count < 2) {
                _mtx_data_entry.lock();
                std::unordered_map<void* ,migratable_data_entry_t*>::const_iterator got = _data_entries.find(tmp_tgt_ptr);
                found = got!=_data_entries.end() ? 1 : 0;
                if(found) {
                    migratable_data_entry_t* entry  = got->second;
                    task->arg_sizes[i]              = entry->size;
                    task->arg_hst_pointers[i]       = entry->hst_ptr;
                }
                _mtx_data_entry.unlock();
                count++;
                if(!found) {
                    // There seems to be a race condition in internal mapping (libomptarget) from source to target pointers (that might be reused) and the kernel call if using multiple threads 
                    // Workaround: wait a small amount of time and try again once more (i know it is ugly but hard to fix that race here)
                    usleep(1000);
                }
            }
#if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            if(cur_time > 0) {
                atomic_add_dbl(_time_data_submit_sum, cur_time);
                _time_data_submit_count++;
            }
#endif
            if(!found) {
                // something went wrong here
                /*RELP("Error: lookup_hst_pointers - Cannot find mapping for arg_tgt: " DPxMOD ", type: %ld, literal: %d, from: %d\n", 
                    DPxPTR(tmp_tgt_ptr),
                    tmp_type,
                    is_lit,
                    is_from);*/
                return CHAM_FAILURE;
            } else {
                print_arg_info_w_tgt("lookup_hst_pointers", task, i);
            }
        }
    }

    // // clean up data entries again to avoid problems
    // _mtx_data_entry.lock();
    // for(int i = 0; i < task->arg_num; i++) {
    //     // get type and pointer
    //     int64_t tmp_type    = task->arg_types[i];
    //     void * tmp_tgt_ptr  = task->arg_tgt_pointers[i];
    //     int is_lit          = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;

    //     if(!(tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL)) {
    //         for(auto &entry : _data_entries) {
    //             if(entry->tgt_ptr == tmp_tgt_ptr) {
    //                     // remove it if found
    //                     free(entry->tgt_ptr);
    //                     mem_allocated -= entry->size;
    //                     _data_entries.remove(entry);
    //                 break;
    //             }
    //         }   
    //     }
    // }
    // _mtx_data_entry.unlock();

    DBP("lookup_hst_pointers (exit)\n");
    return CHAM_SUCCESS;
}

int32_t execute_target_task(cham_migratable_task_t *task) {
    DBP("execute_target_task (enter) - task_entry (task_id=%ld): " DPxMOD ", is_migrated: %d, is_replicated: %d\n", task->task_id, DPxPTR(task->tgt_entry_ptr), task->is_migrated_task, task->is_replicated_task);
    int32_t gtid = __ch_get_gtid();
    // Use libffi to launch execution.
    ffi_cif cif;

    // Add some noise here when executing the task
#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_change_freq_for_execution) {
        int32_t noise_time = cham_t_status.cham_t_callback_change_freq_for_execution(task, _load_info_ranks[chameleon_comm_rank], _total_created_tasks_per_rank);
        // make the process slower by sleep
        DBP("execute_target_task - noise_time = %d\n", noise_time);
        if (noise_time != 0)
            usleep(noise_time);
    }
#endif

    // All args are references.
    std::vector<ffi_type *> args_types(task->arg_num, &ffi_type_pointer);

    std::vector<void *> args(task->arg_num);
    std::vector<void *> ptrs(task->arg_num);

    for (int32_t i = 0; i < task->arg_num; ++i) {
        // int64_t tmp_type        = task->arg_types[i];
        // int64_t is_literal      = (tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL);
        // int64_t is_implicit     = (tmp_type & CHAM_OMP_TGT_MAPTYPE_IMPLICIT);
        // int64_t is_to           = (tmp_type & CHAM_OMP_TGT_MAPTYPE_TO);
        // int64_t is_from         = (tmp_type & CHAM_OMP_TGT_MAPTYPE_FROM);
        // int64_t is_prt_obj      = (tmp_type & CHAM_OMP_TGT_MAPTYPE_PTR_AND_OBJ);
        
        // always apply offset in case of array sections
        ptrs[i] = (void *)((intptr_t)task->arg_hst_pointers[i] + task->arg_tgt_offsets[i]);
        args[i] = &ptrs[i];

        print_arg_info("execute_target_task", task, i);
    }
    
    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, task->arg_num, &ffi_type_void, &args_types[0]);

    if(status != FFI_OK) {
        printf("Unable to prepare target launch!\n");
        return CHAM_FAILURE;
    }

    cham_migratable_task_t *prior_task = __thread_data[gtid].current_task;

#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_task_schedule) {
        if(prior_task) {
            cham_t_status.cham_t_callback_task_schedule(
                task,
                task->is_remote_task ? cham_t_task_remote : cham_t_task_local,
                &(task->task_tool_data), 
                cham_t_task_yield,
                prior_task,
                prior_task->is_remote_task ? cham_t_task_remote : cham_t_task_local,
                &(prior_task->task_tool_data));
        } else {
            cham_t_status.cham_t_callback_task_schedule(
                task, 
                task->is_remote_task ? cham_t_task_remote : cham_t_task_local,
                &(task->task_tool_data), 
                cham_t_task_start,
                nullptr, 
                cham_t_task_local,
                nullptr);
        }
    }
#endif

    __thread_data[gtid].current_task = task;
    void (*entry)(void);
    *((void**) &entry) = ((void*) task->tgt_entry_ptr);
    // use host pointers here
    ffi_call(&cif, entry, NULL, &args[0]);

#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_task_schedule) {
        if(prior_task) {
            cham_t_status.cham_t_callback_task_schedule(
                task, 
                task->is_remote_task ? cham_t_task_remote : cham_t_task_local,
                &(task->task_tool_data), 
                cham_t_task_end,
                prior_task,
                prior_task->is_remote_task ? cham_t_task_remote : cham_t_task_local,
                &(prior_task->task_tool_data));
        } else {
            cham_t_status.cham_t_callback_task_schedule(
                task, 
                task->is_remote_task ? cham_t_task_remote : cham_t_task_local,
                &(task->task_tool_data), 
                cham_t_task_end,
                nullptr, 
                cham_t_task_local,
                nullptr);
        }
    }
#endif
    // switch back to prior task or null
    __thread_data[gtid].current_task = prior_task;

    return CHAM_SUCCESS;
}

inline int32_t process_replicated_local_task() {
    DBP("process_replicated_local_task (enter)\n");
    cham_migratable_task_t *replicated_task = nullptr;

    if(_replicated_local_tasks.empty())
        return CHAM_REPLICATED_TASK_NONE;

    replicated_task = _replicated_local_tasks.pop_back();

    if(replicated_task==nullptr)
        return CHAM_REPLICATED_TASK_NONE;

    bool expected = false;
    bool desired = true;


    DBP("process_replicated_local_task - task %d was reserved for local execution\n", replicated_task->task_id);

#ifdef TRACE
    static int event_process_replicated_local = -1;
    static const std::string event_process_replicated_name = "process_replicated_local";
    if(event_process_replicated_local == -1)
        int ierr = VT_funcdef(event_process_replicated_name.c_str(), VT_NOCLASS, &event_process_replicated_local);
    VT_BEGIN_CONSTRAINED(event_process_replicated_local);
#endif

#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
#endif

#if CHAM_REPLICATION_MODE==2 || CHAM_REPLICATION_MODE==3 
        //cancel task on remote ranks
        cancel_offloaded_task(replicated_task);
#endif

        int32_t res = execute_target_task(replicated_task);
        if(res != CHAM_SUCCESS)
            handle_error_en(1, "execute_target_task - remote");
#if CHAM_STATS_RECORD
        cur_time = omp_get_wtime()-cur_time;
        atomic_add_dbl(_time_task_execution_replicated_sum, cur_time);
        _time_task_execution_replicated_count++;
#endif

        if(!replicated_task->is_migrated_task) {
          int tmp = _num_replicated_local_tasks_outstanding_compute-1;
          _num_replicated_local_tasks_outstanding_compute = std::max(0, tmp);
        }
#if CHAM_STATS_RECORD
        _num_executed_tasks_replicated_local++;
#endif

        // mark locally created task finished
        #if CHAMELEON_ENABLE_FINISHED_TASK_TRACKING
        _unfinished_locally_created_tasks.remove(replicated_task->task_id);
        #endif
        _map_overall_tasks.erase(replicated_task->task_id);

        _num_local_tasks_outstanding--;
        assert(_num_local_tasks_outstanding>=0);
        DBP("process_replicated_task - decrement local outstanding count for task %ld new count %ld\n", replicated_task->task_id, _num_local_tasks_outstanding.load());

        // handle external finish callback
        if(replicated_task->cb_task_finish_func_ptr) {
            replicated_task->cb_task_finish_func_ptr(replicated_task->cb_task_finish_func_param);
        }

#ifdef TRACE
        VT_END_W_CONSTRAINED(event_process_replicated_local);
#endif

    return CHAM_REPLICATED_TASK_SUCCESS;

    DBP("process_replicated_local_task (exit)\n");
}

inline int32_t process_replicated_remote_task() {
    DBP("process_replicated_remote_task (enter)\n");
    cham_migratable_task_t *replicated_task = nullptr;
   
    if(_replicated_remote_tasks.empty())
        return CHAM_REPLICATED_TASK_NONE;
        
    replicated_task = _replicated_remote_tasks.pop_front();
    
    if(replicated_task==nullptr)
        return CHAM_REPLICATED_TASK_NONE;

#ifdef TRACE
    static int event_process_replicated_remote = -1;
    static const std::string event_process_replicated_name = "process_replicated_remote";
    if(event_process_replicated_remote == -1)
        int ierr = VT_funcdef(event_process_replicated_name.c_str(), VT_NOCLASS, &event_process_replicated_remote);
    VT_BEGIN_CONSTRAINED(event_process_replicated_remote);
#endif

#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
#endif

    int32_t res = execute_target_task(replicated_task);
    if(res != CHAM_SUCCESS)
        handle_error_en(1, "execute_target_task - remote");
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime() - cur_time;
    atomic_add_dbl(_time_task_execution_replicated_sum, cur_time);
    _time_task_execution_replicated_count++;
#endif

#if CHAM_STATS_RECORD
    _num_executed_tasks_replicated_remote++;
#endif

    //_map_tag_to_remote_task.erase(replicated_task->task_id);
    _map_overall_tasks.erase(replicated_task->task_id);

    if(replicated_task->HasAtLeastOneOutput()){
        // just schedule it for sending back results if there is at least 1 output
        _remote_tasks_send_back.push_back(replicated_task);
    }
    else{
        _num_remote_tasks_outstanding--;
        DBP("process_replicated_task - decrement remote outstanding count for task %ld\n", replicated_task->task_id);
    }
#ifdef TRACE
    VT_END_W_CONSTRAINED(event_process_replicated_remote);
#endif

    return CHAM_REPLICATED_TASK_SUCCESS;
  
    DBP("process_replicated_remote_task (exit)\n");
}

inline int32_t process_replicated_migrated_task() {
    DBP("process_replicated_remote_task (enter)\n");
    cham_migratable_task_t *replicated_task = nullptr;

    if(_replicated_migrated_tasks.empty())
        return CHAM_REPLICATED_TASK_NONE;

    replicated_task = _replicated_migrated_tasks.pop_front();

    if(replicated_task == nullptr)
        return CHAM_REPLICATED_TASK_NONE;

#ifdef TRACE
    static int event_process_replicated_migrated = -1;
    static const std::string event_process_replicated_name = "process_replicated_migrated";
    if(event_process_replicated_migrated == -1)
        int ierr = VT_funcdef(event_process_replicated_name.c_str(), VT_NOCLASS, &event_process_replicated_migrated);
    VT_BEGIN_CONSTRAINED(event_process_replicated_migrated);
#endif

#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
#endif
    int32_t res = execute_target_task(replicated_task);
    if(res != CHAM_SUCCESS)
        handle_error_en(1, "execute_target_task - remote");
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime() - cur_time;
    atomic_add_dbl(_time_task_execution_replicated_sum, cur_time);
    _time_task_execution_replicated_count++;
#endif

#if CHAM_STATS_RECORD
    _num_executed_tasks_replicated_remote++;
#endif

    //_map_tag_to_remote_task.erase(replicated_task->task_id);
    _map_overall_tasks.erase(replicated_task->task_id);

    if(replicated_task->HasAtLeastOneOutput()){
        // just schedule it for sending back results if there is at least 1 output
        _remote_tasks_send_back.push_back(replicated_task);
    }
    else{
        _num_remote_tasks_outstanding--;
        DBP("process_replicated_task - decrement remote outstanding count for task %ld\n", replicated_task->task_id);
    }
#ifdef TRACE
    VT_END_W_CONSTRAINED(event_process_replicated_migrated);
#endif

    return CHAM_REPLICATED_TASK_SUCCESS;

    DBP("process_replicated_migrated_task (exit)\n");
}


inline int32_t process_remote_task() {
    DBP("process_remote_task (enter)\n");
    
    cham_migratable_task_t *task = nullptr;

    if(_stolen_remote_tasks.empty())
        return CHAM_REMOTE_TASK_NONE;

    task = _stolen_remote_tasks.pop_front();

    if(!task)
        return CHAM_REMOTE_TASK_NONE;

    DBP("process_remote_task - task_id: %ld\n", task->task_id);

    int is_migrated= task->is_migrated_task;
#ifdef TRACE
    static int event_process_remote = -1;
    static const std::string event_process_remote_name = "process_remote";
    if( event_process_remote == -1) 
        int ierr = VT_funcdef(event_process_remote_name.c_str(), VT_NOCLASS, &event_process_remote);

    static int event_process_replicated_remote_hp = -1;
    static const std::string event_process_replicated_hp_name = "process_replicated_remote_highprio";
    if( event_process_replicated_remote_hp == -1)
        int ierr = VT_funcdef(event_process_replicated_hp_name.c_str(), VT_NOCLASS, &event_process_replicated_remote_hp);

    if(!task->is_migrated_task) {
        VT_BEGIN_CONSTRAINED(event_process_replicated_remote_hp);
    }
    else {
        VT_BEGIN_CONSTRAINED(event_process_remote);
    }
#endif

    // execute region now
#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
#endif
    int32_t res = execute_target_task(task);
    if(res != CHAM_SUCCESS)
        handle_error_en(1, "execute_target_task - remote");
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    if(is_migrated) {
      atomic_add_dbl(_time_task_execution_stolen_sum, cur_time);
      _time_task_execution_stolen_count++;
    }
    else {
      atomic_add_dbl(_time_task_execution_replicated_sum, cur_time);
      _time_task_execution_replicated_count++;
    }
#endif
 
    _map_tag_to_remote_task.erase(task->task_id);
    _map_overall_tasks.erase(task->task_id);

    if(task->HasAtLeastOneOutput()) {
        // just schedule it for sending back results if there is at least 1 output
        _remote_tasks_send_back.push_back(task);
    } else {
        // we can now decrement outstanding counter because there is nothing to send back
        _num_remote_tasks_outstanding--;
        DBP("process_remote_task - decrement stolen outstanding count for task %ld\n", task->task_id);
        // TODO: how to handle external finish callback? maybe handshake with owner?
        free_migratable_task(task, true);
    }

#if CHAM_STATS_RECORD
    if(is_migrated)
      _num_executed_tasks_stolen++;
    else
    	_num_executed_tasks_replicated_remote++;
#endif
#ifdef TRACE
    if(is_migrated) {
        VT_END_W_CONSTRAINED(event_process_replicated_remote_hp);
    }
    else {
        VT_END_W_CONSTRAINED(event_process_remote);
    }
#endif
    return CHAM_REMOTE_TASK_SUCCESS;
}

inline int32_t process_local_task() {
    cham_migratable_task_t *task = _local_tasks.pop_front();
    if(!task)
        return CHAM_LOCAL_TASK_NONE;

#ifdef TRACE
    static int event_process_local = -1;
    static const std::string event_process_local_name = "process_local";
    if( event_process_local == -1)
        int ierr = VT_funcdef(event_process_local_name.c_str(), VT_NOCLASS, &event_process_local);
#endif

    // execute region now
    DBP("process_local_task - task_id: %ld\n", task->task_id);
#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
#endif
#ifdef TRACE
    VT_BEGIN_CONSTRAINED(event_process_local);
#endif
    int32_t res = execute_target_task(task);
    if(res != CHAM_SUCCESS)
        handle_error_en(1, "execute_target_task - local");
#ifdef TRACE
    VT_END_W_CONSTRAINED(event_process_local);
#endif
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_task_execution_local_sum, cur_time);
    _time_task_execution_local_count++;
#endif
    // mark locally created task finished
    #if CHAMELEON_ENABLE_FINISHED_TASK_TRACKING
    _unfinished_locally_created_tasks.remove(task->task_id);
    #endif
    _map_overall_tasks.erase(task->task_id);

    // it is save to decrement counter after local execution
    _num_local_tasks_outstanding--;
    assert(_num_local_tasks_outstanding>=0);
    DBP("process_local_task - decrement local outstanding count for task %ld new %d\n", task->task_id, _num_local_tasks_outstanding.load());

    // handle external finish callback
    if(task->cb_task_finish_func_ptr) {
        task->cb_task_finish_func_ptr(task->cb_task_finish_func_param);
    }

#if CHAM_STATS_RECORD
    _num_executed_tasks_local++;
#endif
    free_migratable_task(task, false);
    return CHAM_LOCAL_TASK_SUCCESS;
}

#pragma endregion Fcns for Lookups and Execution

#ifdef __cplusplus
}
#endif
