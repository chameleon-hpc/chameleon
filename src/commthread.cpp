#include "commthread.h"
#include "chameleon_common.h"
#include "cham_statistics.h"
#include "cham_strategies.h"
#include "request_manager.h"
#include "chameleon_tools.h"
#include "chameleon_tools_internal.h"

#include <pthread.h>
#include <signal.h>
#include <numeric>
#include <sched.h>
#include <hwloc.h>
#include <hwloc/glibc-sched.h>
#include <functional>
#include <set>

#ifdef TRACE
#include "VT.h"
#endif

#define CHAM_SLEEP_TIME_MICRO_SECS 5

#pragma region Variables
// communicator for remote task requests
MPI_Comm chameleon_comm;
// communicator for sending back mapped values
MPI_Comm chameleon_comm_mapped;
// communicator for cancelling offloaded tasks
MPI_Comm chameleon_comm_cancel;
// communicator for load information
MPI_Comm chameleon_comm_load;

int chameleon_comm_rank = -1;
int chameleon_comm_size = -1;

//request manager for MPI requests
RequestManager request_manager_receive;
RequestManager request_manager_send;
RequestManager request_manager_cancel;

//"trash buffer" for late receives (i.e. a replicated task is already processed locally)
void *trash_buffer = nullptr;
int cur_trash_buffer_size = 0;

// array that holds image base addresses
std::vector<intptr_t> _image_base_addresses;

// list with local task entries
// these can either be executed here or offloaded to a different rank (i.e., become replicated tasks)
thread_safe_task_list_t _local_tasks;
std::atomic<int32_t> _num_local_tasks_outstanding(0);

// list with stolen task entries that should be executed but may be cancelled if task is executed on origin rank
thread_safe_task_list_t _stolen_remote_tasks;
std::atomic<int32_t> _num_stolen_tasks_outstanding(0);

// list with replicated (i.e. offloaded) task entries
// these can either be executed remotely or locally
thread_safe_task_list_t _replicated_tasks;
std::atomic<int32_t> _num_replicated_tasks_outstanding(0);

// list with stolen task entries that need output data transfer
thread_safe_task_list_t _stolen_remote_tasks_send_back;

// map that maps tag ids back to local tasks that have been offloaded and expect result data
thread_safe_task_map_t _map_offloaded_tasks_with_outputs;
// map that maps tag ids back to stolen tasks
thread_safe_task_map_t _map_tag_to_stolen_task;
// mapping of all active task ids and task
thread_safe_task_map_t _map_overall_tasks;

// ====== Info about outstanding jobs (local & stolen) ======
// extern std::mutex _mtx_outstanding_jobs;
std::vector<int32_t> _outstanding_jobs_ranks;
std::atomic<int32_t> _outstanding_jobs_local(0);
std::atomic<int32_t> _outstanding_jobs_sum(0);
// counter for current number of offloaded tasks
std::atomic<int> _num_offloaded_tasks_outstanding(0);

// ====== Info about real load that is open or is beeing processed ======
std::vector<int32_t> _load_info_ranks;
// for now use a single mutex for box info
std::mutex _mtx_load_exchange;

#if OFFLOAD_BLOCKING
// only enable offloading when a task has finished on local rank (change has been made)
std::atomic<int32_t> _offload_blocked(0);
#endif

// === Constants
const int32_t MAX_BUFFER_SIZE_OFFLOAD_ENTRY = 20480; // 20 KB for testing

// ============== Thread Section ===========
std::mutex _mtx_comm_threads_started;
std::atomic<int> _comm_threads_started(0);
std::atomic<int> _comm_thread_load_exchange_happend(0);
std::atomic<int> _comm_thread_service_stopped(0);

std::mutex _mtx_comm_threads_ended;
std::atomic<int> _comm_threads_ended_count(0);

// flag that signalizes comm threads to abort their work
std::atomic<int> _flag_abort_threads(0);
int tag_counter_send_tasks = 0;

// variables to indicate when it is save to break out of taskwait
std::mutex _mtx_taskwait;
std::atomic<int> _flag_comm_threads_sleeping(1);

std::atomic<int> _num_threads_involved_in_taskwait(INT_MAX);
std::atomic<int32_t> _num_threads_idle(0);
std::atomic<int> _num_ranks_not_completely_idle(INT_MAX);

pthread_t           _th_service_actions;
std::atomic<int>    _th_service_actions_created(0);
pthread_cond_t      _th_service_actions_cond    = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     _th_service_actions_mutex   = PTHREAD_MUTEX_INITIALIZER;
#pragma endregion Variables

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Forward Declarations
// ================================================================================
// Forward declartion of internal functions (just called inside shared library)
// ================================================================================
void* encode_send_buffer(cham_migratable_task_t **tasks, int32_t num_tasks, int32_t *buffer_size);
void decode_send_buffer(void *buffer, int mpi_tag, int32_t *num_tasks, std::vector<cham_migratable_task_t*> &tasks);

short pin_thread_to_last_core(int n_last_core);
void offload_action(cham_migratable_task_t **tasks, int32_t num_tasks, int target_rank);
int32_t offload_tasks_to_rank(cham_migratable_task_t **tasks, int32_t num_tasks, int target_rank);

static void send_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void receive_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void receive_handler_data(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void send_back_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void receive_back_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void receive_back_trash_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);

#pragma endregion Forward Declarations

#pragma region Start/Stop/Pin Communication Threads
int32_t start_communication_threads() {
    if(_comm_threads_started)
        return CHAM_SUCCESS;
    
    _mtx_comm_threads_started.lock();
    // need to check again
    if(_comm_threads_started) {
        _mtx_comm_threads_started.unlock();
        return CHAM_SUCCESS;
    }

    #if !THREAD_ACTIVATION
    #if CHAM_STATS_RECORD && CHAM_STATS_PER_SYNC_INTERVAL
        cham_stats_reset_for_sync_cycle();
    #endif
    #endif

    DBP("start_communication_threads (enter)\n");
    // set flag to avoid that threads are directly aborting
    _flag_abort_threads = 0;
    // indicating that this has not happend yet for the current sync cycle
    _comm_thread_load_exchange_happend = 0;

    // explicitly make threads joinable to be portable
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    int err;
    err = pthread_create(&_th_service_actions, &attr, comm_thread_action, NULL);
    if(err != 0)
        handle_error_en(err, "pthread_create - _th_service_actions");

    pthread_mutex_lock(&_th_service_actions_mutex);
    while (_th_service_actions_created == 0) {
        pthread_cond_wait(&_th_service_actions_cond, &_th_service_actions_mutex);
    }
    pthread_mutex_unlock(&_th_service_actions_mutex);

    // set flag to ensure that only a single thread is creating communication threads
    _comm_threads_started = 1;
    _mtx_comm_threads_started.unlock();
    DBP("start_communication_threads (exit)\n");
    return CHAM_SUCCESS;
}

int32_t chameleon_wake_up_comm_threads() {
    // check wether communication threads have already been started. Otherwise do so.
    // Usually that should not be necessary if done in init
    // start_communication_threads();

    // if threads already awake
    if(!_flag_comm_threads_sleeping)
        return CHAM_SUCCESS;
    
    _mtx_taskwait.lock();
    // need to check again
    if(!_flag_comm_threads_sleeping) {
        _mtx_taskwait.unlock();
        return CHAM_SUCCESS;
    }

    DBP("chameleon_wake_up_comm_threads (enter) - _flag_comm_threads_sleeping = %d\n", _flag_comm_threads_sleeping.load());

    #if CHAM_STATS_RECORD && CHAM_STATS_PER_SYNC_INTERVAL
        cham_stats_reset_for_sync_cycle();
    #endif
    // determine or set values once
    _num_threads_involved_in_taskwait   = omp_get_num_threads();
    _num_threads_idle                   = 0;
    DBP("chameleon_wake_up_comm_threads       - _num_threads_idle =============> reset: %d\n", _num_threads_idle.load());

    // indicating that this has not happend yet for the current sync cycle
    _comm_thread_load_exchange_happend  = 0;
    _comm_thread_service_stopped        = 0;
    _flag_comm_threads_sleeping         = 0;

    #if defined(TRACE) && ENABLE_TRACING_FOR_SYNC_CYCLES
    _num_sync_cycle++;
    if(_num_sync_cycle >= ENABLE_TRACE_FROM_SYNC_CYCLE && _num_sync_cycle <= ENABLE_TRACE_TO_SYNC_CYCLE) {
        _tracing_enabled = 1;
    } else {
        _tracing_enabled = 0;
    }
    #endif

    _mtx_taskwait.unlock();
    DBP("chameleon_wake_up_comm_threads (exit) - _flag_comm_threads_sleeping = %d\n", _flag_comm_threads_sleeping.load());
    return CHAM_SUCCESS;
}

int32_t stop_communication_threads() {

    // ===> Not necessary any more... should just be done in finalize call
    #if !THREAD_ACTIVATION
    _mtx_comm_threads_ended.lock();
    // increment counter
    _comm_threads_ended_count++;

    // check whether it is the last thread that comes along here
    // TODO: omp_get_num_threads might not be the correct choice in all cases.. need to check
    if(_comm_threads_ended_count < omp_get_num_threads()) {
        _mtx_comm_threads_ended.unlock();
        return CHAM_SUCCESS;
    }
    #endif

    DBP("stop_communication_threads (enter)\n");
    int err = 0;
    // safer that way because it leads to severe problems if threads are canceled inside a MPI communication
    _flag_abort_threads = 1;
    // then wait for all threads to finish
    err = pthread_join(_th_service_actions, NULL);
    if(err != 0)    handle_error_en(err, "stop_communication_threads - _th_service_actions");

    // should be save to reset flags and counters here
    _comm_threads_started = 0;
    #if !THREAD_ACTIVATION
    _comm_thread_load_exchange_happend = 0;
    _comm_threads_ended_count = 0;
    
    #ifdef CHAM_DEBUG
    DBP("stop_communication_threads - still mem_allocated = %ld\n", (long)mem_allocated);
    mem_allocated = 0;
    #endif

    _th_service_actions_created             = 0;
    _num_threads_involved_in_taskwait       = INT_MAX;
    _num_threads_idle                       = 0;
    #endif

    #if CHAM_STATS_RECORD && CHAM_STATS_PRINT && !CHAM_STATS_PER_SYNC_INTERVAL
    cham_stats_print_stats();
    #endif
    
    DBP("stop_communication_threads (exit)\n");
    _mtx_comm_threads_ended.unlock();
    return CHAM_SUCCESS;
}

int32_t put_comm_threads_to_sleep() {
    _mtx_comm_threads_ended.lock();
    // increment counter
    _comm_threads_ended_count++;

    // check whether it is the last thread that comes along here
    // TODO: omp_get_num_threads might not be the correct choice in all cases.. need to check
    if(_comm_threads_ended_count < _num_threads_involved_in_taskwait) {
        _mtx_comm_threads_ended.unlock();
        return CHAM_SUCCESS;
    }

    DBP("put_comm_threads_to_sleep (enter) - _flag_comm_threads_sleeping = %d\n", _flag_comm_threads_sleeping.load());
    #ifdef CHAM_DEBUG
        DBP("put_comm_threads_to_sleep - still mem_allocated = %ld\n", (long)mem_allocated);
        mem_allocated = 0;
    #endif

    #if CHAM_STATS_RECORD && CHAM_STATS_PRINT && CHAM_STATS_PER_SYNC_INTERVAL
        cham_stats_print_stats();
    #endif

    _flag_comm_threads_sleeping             = 1;
    // wait until thread sleeps
    while(!_comm_thread_service_stopped) {
        usleep(CHAM_SLEEP_TIME_MICRO_SECS);
    }
    // DBP("put_comm_threads_to_sleep - service thread stopped = %d\n", _comm_thread_service_stopped);
    _comm_threads_ended_count               = 0;
    _comm_thread_load_exchange_happend      = 0;
    _num_threads_involved_in_taskwait       = INT_MAX;
    _num_threads_idle                       = 0;
    DBP("put_comm_threads_to_sleep  - _num_threads_idle =============> reset: %d\n", _num_threads_idle.load());
    _num_ranks_not_completely_idle          = INT_MAX;
    DBP("put_comm_threads_to_sleep - new _num_ranks_not_completely_idle: INT_MAX\n");

    DBP("put_comm_threads_to_sleep (exit)\n");
    _mtx_comm_threads_ended.unlock();
    return CHAM_SUCCESS;
}

void print_cpu_set(cpu_set_t set) {
    std::string val("");
    for(long i = 0; i < 48; i++) {
        if (CPU_ISSET(i, &set)) {
            val += std::to_string(i)  + ",";
        }
    }
    RELP("Process/thread affinity to cores %s\n", val.c_str());
}

short pin_thread_to_last_core(int n_last_core) {
    int err;
    int s, j;
    pthread_t thread;
    cpu_set_t current_cpuset;
    cpu_set_t new_cpu_set;
    cpu_set_t final_cpu_set;    

    // somehow this only reflects binding of current thread. Problem when OpenMP already pinned threads due to OMP_PLACES and OMP_PROC_BIND. 
    // then comm thread only get cpuset to single core --> overdecomposition of core with computational and communication thread  
    // err = sched_getaffinity(getpid(), sizeof(cpu_set_t), &current_cpuset);
    // if(err != 0)
    //     handle_error_en(err, "sched_getaffinity");

    hwloc_topology_t topology;
    hwloc_bitmap_t set, hwlocset;
    err = hwloc_topology_init(&topology);
    err = hwloc_topology_load(topology);
    hwlocset = hwloc_bitmap_alloc();
    err = hwloc_get_proc_cpubind(topology, getpid(), hwlocset, 0);
    hwloc_cpuset_to_glibc_sched_affinity(topology, hwlocset, &current_cpuset, sizeof(current_cpuset));
    hwloc_bitmap_free(hwlocset);
    
    // also get the number of processing units (here)
    int depth = hwloc_get_type_depth(topology, HWLOC_OBJ_CORE);
    if(depth == HWLOC_TYPE_DEPTH_UNKNOWN) {
        handle_error_en(1001, "hwloc_get_type_depth");
    }
    const long n_physical_cores = hwloc_get_nbobjs_by_depth(topology, depth);
    const long n_logical_cores = sysconf( _SC_NPROCESSORS_ONLN );

    hwloc_topology_destroy(topology);
    
    // get last hw thread of current cpuset
    long max_core_set = -1;
    int count_last = 0;

    for (long i = n_logical_cores; i >= 0; i--) {
        if (CPU_ISSET(i, &current_cpuset)) {
            // DBP("Last core/hw thread in cpuset is %ld\n", i);
            max_core_set = i;
            count_last++;
            if(count_last >= n_last_core)
                break;
        }
    }

    // set affinity mask to last core or all hw threads on specific core 
    CPU_ZERO(&new_cpu_set);
    if(max_core_set < n_physical_cores) {
        // Case: there are no hyper threads
        RELP("COMM_THREAD: Setting thread affinity to core %ld\n", max_core_set);
        CPU_SET(max_core_set, &new_cpu_set);
    } else {
        // Case: there are at least 2 HT per core
        std::string cores(std::to_string(max_core_set));
        CPU_SET(max_core_set, &new_cpu_set);
        for(long i = max_core_set-n_physical_cores; i >= 0; i-=n_physical_cores) {
            cores = std::to_string(i)  + "," + cores;
            CPU_SET(i, &new_cpu_set);
        }
        RELP("COMM_THREAD: Setting thread affinity to cores %s\n", cores.c_str());
    }
    // get current thread to set affinity for    
    thread = pthread_self();
    err = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &new_cpu_set);
    if (err != 0)
        handle_error_en(err, "pthread_setaffinity_np");

    // // ===== DEBUG
    // // verify that pinning worked
    // err = pthread_getaffinity_np(thread, sizeof(cpu_set_t), &final_cpu_set);
    // if (err != 0)
    //     handle_error_en(err, "pthread_getaffinity_np");

    // std::string final_cores("");
    // for (int j = 0; j < n_logical_cores; j++)
    //     if (CPU_ISSET(j, &final_cpu_set))
    //         final_cores += std::to_string(j) + ",";

    // final_cores.pop_back();
    // RELP("COMM_THREAD: Verifying thread affinity: pinned to cores %s\n", final_cores.c_str());
    // // ===== DEBUG

    return CHAM_SUCCESS;
}
#pragma endregion Start/Stop/Pin Communication Threads

#pragma region Handler
static void handler_noop(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
};

static void send_handler(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    free(buffer);
};

static void receive_handler_data(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    // add tasks to stolen list
    for (int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];        
        _stolen_remote_tasks.push_back(task);
        _map_tag_to_stolen_task.insert(task->task_id, task);
        _map_overall_tasks.insert(task->task_id, task);
    }
}

static void receive_handler(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    DBP("receive_handler - received tasks from rank %d with tag: %d\n", source, tag);
    std::vector<cham_migratable_task_t*> list_tasks;
    int32_t n_tasks = 0;
#if CHAM_STATS_RECORD
    double cur_time_decode, cur_time;
    cur_time_decode = omp_get_wtime();
#endif
    decode_send_buffer(buffer, tag, &n_tasks, list_tasks);
    free(buffer);
#if CHAM_STATS_RECORD
    cur_time_decode = omp_get_wtime()-cur_time_decode;
    atomic_add_dbl(_time_decode_sum, cur_time_decode);
    _time_decode_count++;
#endif

    // add stolen load as soon as possible to avoid wrong decision making
    _mtx_load_exchange.lock();
    _num_stolen_tasks_outstanding += n_tasks;
    DBP("receive_handler - increment stolen outstanding count for tag %d by %d\n", tag, num_tasks);
    trigger_update_outstanding();
    _mtx_load_exchange.unlock();

    // set mpi source + copy pointers to separate array
    cham_migratable_task_t** p_tasks = (cham_migratable_task_t**) malloc(n_tasks*sizeof(cham_migratable_task_t*));
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        list_tasks[i_task]->source_mpi_rank = source;
        p_tasks[i_task] = list_tasks[i_task];
    }

#if OFFLOAD_DATA_PACKING_TYPE > 0
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime();
#endif
#if OFFLOAD_DATA_PACKING_TYPE == 1
    int num_requests = 0;
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        num_requests += list_tasks[i_task]->arg_num;
    }
    MPI_Request *requests = new MPI_Request[num_requests];
    int cur_req_num = 0;
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        cham_migratable_task_t *task = list_tasks[i_task];
        for(int i=0; i<task->arg_num; i++) {
            int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            if(is_lit) {
                int ierr = MPI_Irecv(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, source, tag, chameleon_comm, &requests[cur_req_num]);
                assert(ierr==MPI_SUCCESS);
            } else {
                int ierr = MPI_Irecv(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, source, tag, chameleon_comm, &requests[cur_req_num]);
                assert(ierr==MPI_SUCCESS);
            }
            cur_req_num++;
            print_arg_info("receive_handler - receiving argument", task, i);
        }
    }
#elif OFFLOAD_DATA_PACKING_TYPE == 2
    int num_requests = 1;
    MPI_Request *requests = new MPI_Request[num_requests];
    int tmp_overall_arg_nums = 0;
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        tmp_overall_arg_nums += list_tasks[i_task]->arg_num;
    }

    MPI_Datatype type_mapped_vars;
    MPI_Datatype separate_types[tmp_overall_arg_nums];
    int blocklen[tmp_overall_arg_nums];
    MPI_Aint disp[tmp_overall_arg_nums];
    int ierr = 0;
    int tmp_count = 0;

    for(int i_task = 0; i_task < n_tasks; i_task++) {
        cham_migratable_task_t *task = list_tasks[i_task];
        for(int i=0; i<task->arg_num; i++) {
            separate_types[tmp_count]   = MPI_BYTE;
            blocklen[tmp_count]         = task->arg_sizes[i];
            int is_lit                  = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;

            if(is_lit) {
                ierr = MPI_Get_address(&task->arg_hst_pointers[i], &(disp[tmp_count]));
                // assert(ierr==MPI_SUCCESS);
            }
            else {
                ierr = MPI_Get_address(task->arg_hst_pointers[i], &(disp[tmp_count]));
                // assert(ierr==MPI_SUCCESS);
            }
            tmp_count++;
        }
    }
    ierr = MPI_Type_create_struct(tmp_overall_arg_nums, blocklen, disp, separate_types, &type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_commit(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Irecv(MPI_BOTTOM, 1, type_mapped_vars, source, tag, chameleon_comm, &requests[0]);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_free(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
#endif /* OFFLOAD_DATA_PACKING_TYPE == ... */
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_comm_recv_task_sum, cur_time);
#endif /* CHAM_STATS_RECORD */
    request_manager_receive.submitRequests(tag, 
                                    source,
                                    num_requests, 
                                    requests,
                                    MPI_BLOCKING,
                                    receive_handler_data,
                                    recvData, 
                                    NULL, 
                                    p_tasks,
                                    n_tasks);
    delete[] requests;
#endif /* OFFLOAD_DATA_PACKING_TYPE > 0 */
}

static void send_back_handler(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    DBP("send_back_handler - called for tag %d\n", tag);
    if(buffer)
        free(buffer);
    
    if(tasks) {
        for(int i_task = 0; i_task < num_tasks; i_task++) {
            cham_migratable_task_t *task = tasks[i_task];
            free_migratable_task(task, true);
        }
        free(tasks);
    }
}

static void receive_back_handler(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    DBP("receive_back_handler - receiving output data from rank %d for tag: %d\n", source, tag); 
    cham_migratable_task_t *task_entry = _map_offloaded_tasks_with_outputs.find_and_erase(tag);
    if(task_entry) {
        //only if data is packed, we need to copy it out
        #if OFFLOAD_DATA_PACKING_TYPE == 0
        // copy results back to source pointers with memcpy
        char * cur_ptr = (char*) buffer;
        for(int i = 0; i < task_entry->arg_num; i++) {
            int is_lit      = task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_from     = task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;

            if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                print_arg_info("receive_remote_tasks", task_entry, i);
    
                // we already have information about size and data type
                memcpy(task_entry->arg_hst_pointers[i], cur_ptr, task_entry->arg_sizes[i]);
                cur_ptr += task_entry->arg_sizes[i];
            }
       }
       free(buffer);   
       #endif
        _num_offloaded_tasks_outstanding--;

        // mark locally created task finished
        _unfinished_locally_created_tasks.remove(task_entry->task_id);
        _map_overall_tasks.erase(task_entry->task_id);

        // decrement counter if offloading + receiving results finished
        _mtx_load_exchange.lock();
        _num_local_tasks_outstanding--;
        DBP("receive_back_handler - decrement local outstanding count for task %ld\n", task_entry->task_id);
        trigger_update_outstanding();
        _mtx_load_exchange.unlock();

        free_migratable_task(task_entry, false);
    }
}

static void receive_back_trash_handler(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    DBP("receive_remote_tasks_trash - receiving output data from rank %d for tag into trash: %d\n", source, tag);
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];
        free_migratable_task(task, false);
    }
}
#pragma endregion

#pragma region Offloading / Packing
void cancel_offloaded_task(cham_migratable_task_t *task) {
    DBP("cancel_offloaded_task - canceling offloaded task, task_id: %ld, target_rank: %d\n", task->task_id, task->target_mpi_rank);
    MPI_Request request;
    // TODO: depends on int32 or int64
    MPI_Isend(&task->task_id, 1, MPI_INTEGER, task->target_mpi_rank, 0, chameleon_comm_cancel, &request);
    request_manager_cancel.submitRequests( 0, task->target_mpi_rank, 1, 
                                           &request,
                                           MPI_BLOCKING, 
                                           handler_noop,
                                           send,   // TODO: special request
                                           nullptr);
}

int32_t offload_tasks_to_rank(cham_migratable_task_t **tasks, int32_t num_tasks, int target_rank) {
    #ifdef TRACE
    static int event_offload = -1;
    std::string event_offload_name = "offload_tasks";
    if(event_offload == -1) 
        int ierr = VT_funcdef(event_offload_name.c_str(), VT_NOCLASS, &event_offload);
    VT_BEGIN_CONSTRAINED(event_offload);
    #endif /* TRACE */

    assert(tasks[0]->sync_commthread_lock.load()==false);

    #if CHAM_DEBUG
    std::string str_task_ids = std::to_string(tasks[0]->task_id);
    for(int i_task = 1; i_task < num_tasks; i_task++) {
        assert(tasks[i_task]->sync_commthread_lock.load()==false);
        str_task_ids.append("," + std::to_string(tasks[i_task]->task_id));
    }
    DBP("offload_tasks_to_rank (enter) - num_tasks: %d, target_rank: %d, task_ids: %s\n", num_tasks, target_rank, str_task_ids.c_str());
    #endif
    
    #if CHAM_REPLICATION_MODE > 0
    _num_replicated_tasks_outstanding += num_tasks;
    for(int i = 0; i < num_tasks; i++)
        _replicated_tasks.push_back(tasks[i]);
    #endif /* CHAM_REPLICATION_MODE */
    
    offload_action(tasks, num_tasks, target_rank);
    _num_offloaded_tasks_outstanding += num_tasks;

    #if CHAM_STATS_RECORD
    _num_tasks_offloaded += num_tasks;
    #endif

    DBP("offload_tasks_to_rank (exit)\n");
    
    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_offload);
    #endif
    return CHAM_SUCCESS;
}

void offload_action(cham_migratable_task_t **tasks, int32_t num_tasks, int target_rank) {
    // use unique tag for current offload in this sync cycle
    int tmp_tag = tag_counter_send_tasks++;
    DBP("offload_action (enter) - num_tasks: %d, target_rank: %d mpi_tag: %d\n", num_tasks, target_rank, tmp_tag);

    // store target rank in task
    for(int i = 0; i < num_tasks; i++) {
        tasks[i]->target_mpi_rank = target_rank;
    }

    // encode buffer
    int32_t buffer_size = 0;
    void *buffer = NULL;

#if CHAM_STATS_RECORD
    double cur_time;
    cur_time = omp_get_wtime();
#endif
    buffer = encode_send_buffer(tasks, num_tasks, &buffer_size);
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_encode_sum, cur_time);
    _time_encode_count++;
#endif

#if OFFLOAD_DATA_PACKING_TYPE == 0
    // RELP("Packing Type: Buffer\n");
    int n_requests = 1;
#elif OFFLOAD_DATA_PACKING_TYPE == 1
    // RELP("Packing Type: Zero Copy\n");
    int n_requests = 1;
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        n_requests += tasks[i_task]->arg_num;
    }
#elif OFFLOAD_DATA_PACKING_TYPE == 2
    // RELP("Packing Type: Zero Copy Single Message\n");
    int n_requests = 2;
#endif

    // send data to target rank
    DBP("offload_action - sending data to target rank %d with mpi_tag: %d\n", target_rank, tmp_tag);
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime();
#endif
    MPI_Request *requests = new MPI_Request[n_requests];
    MPI_Isend(buffer, buffer_size, MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[0]);

#if OFFLOAD_DATA_PACKING_TYPE == 1
    int cur_req_index = 1;
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];
        for(int i=0; i<task->arg_num; i++) {
            int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            if(is_lit) {
                MPI_Isend(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[cur_req_index]);
            }
            else{
                MPI_Isend(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[cur_req_index]);
            }
            cur_req_index++;
            print_arg_info("offload_action - sending argument", task, i);
        }
    }
#elif OFFLOAD_DATA_PACKING_TYPE == 2
    int tmp_overall_arg_nums = 0;
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        tmp_overall_arg_nums += tasks[i_task]->arg_num;
    }

    MPI_Datatype type_mapped_vars;
    MPI_Datatype separate_types[tmp_overall_arg_nums];
    int blocklen[tmp_overall_arg_nums];
    MPI_Aint disp[tmp_overall_arg_nums];
    int ierr = 0;
    int tmp_count = 0;

    for(int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];

        for(int i=0; i<task->arg_num; i++) {
            separate_types[tmp_count]   = MPI_BYTE;
            blocklen[tmp_count]         = task->arg_sizes[i];
            int is_lit                  = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            
            if(is_lit) {
                ierr = MPI_Get_address(&task->arg_hst_pointers[i], &(disp[tmp_count]));
                // assert(ierr==MPI_SUCCESS);
            }
            else {
                ierr = MPI_Get_address(task->arg_hst_pointers[i], &(disp[tmp_count]));
                // assert(ierr==MPI_SUCCESS);
            }
            tmp_count++;
        }
    }
    ierr = MPI_Type_create_struct(tmp_overall_arg_nums, blocklen, disp, separate_types, &type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_commit(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Isend(MPI_BOTTOM, 1, type_mapped_vars, target_rank, tmp_tag, chameleon_comm, &requests[1]);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_free(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
#endif
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];
        if(task->HasAtLeastOneOutput()) {
            _map_offloaded_tasks_with_outputs.insert(task->task_id, task);
        }
    }
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_comm_send_task_sum, cur_time);
    _time_comm_send_task_count++;
#endif
    request_manager_send.submitRequests(tmp_tag, target_rank, n_requests, 
                                requests,
                                MPI_BLOCKING,
                                send_handler,
                                send,
                                buffer);
    delete[] requests;
    DBP("offload_action (exit)\n");
}

void * encode_send_buffer(cham_migratable_task_t **tasks, int32_t num_tasks, int32_t *buffer_size) {
    #ifdef TRACE
    static int event_encode = -1;
    std::string event_encode_name = "encode";
    if(event_encode == -1) 
        int ierr = VT_funcdef(event_encode_name.c_str(), VT_NOCLASS, &event_encode);
    VT_BEGIN_CONSTRAINED(event_encode);
    #endif

    DBP("encode_send_buffer (enter) - num_tasks: %d\n", num_tasks);

    // FORMAT:
    //      0. number of tasks
    // 
    //      Then for each task:
    //      1. target function pointer = address (intptr_t)
    //      2. image index
    //      3. offset of entry point inside image
    //      4. task id
    //      5. number of arguments = int32_t
    //      6. array with argument sizes = n_args * int64_t
    //      7. array with argument offsets = n_args * int64_t
    //      8. array with argument types = n_args * int64_t
    //      9. array with values (only for OFFLOAD_DATA_PACKING_TYPE == 0 )
    //     10. annotations
    //     11. tool data if available

    int total_size = sizeof(int32_t);                   // 0. number of tasks

    for (int i = 0; i < num_tasks; i++) {
        total_size += sizeof(intptr_t)              // 1. target entry pointer
            + sizeof(int32_t)                       // 2. img index
            + sizeof(ptrdiff_t)                     // 3. offset inside image
            + sizeof(TYPE_TASK_ID)                  // 4. task id
            + sizeof(int32_t)                       // 5. number of arguments
            + tasks[i]->arg_num * sizeof(int64_t)    // 6. argument sizes
            + tasks[i]->arg_num * sizeof(ptrdiff_t)  // 7. offsets
            + tasks[i]->arg_num * sizeof(int64_t);   // 8. argument types

        #if OFFLOAD_DATA_PACKING_TYPE == 0
        for(int i_arg = 0; i_arg < task[i]->arg_num; i_arg++) {
            total_size += task[i]->arg_sizes[i_arg];
        }
        #endif /* OFFLOAD_DATA_PACKING_TYPE */
    }

    #if CHAM_MIGRATE_ANNOTATIONS
    std::vector<int32_t> annotation_sizes(num_tasks);
    std::vector<void*> annotation_buffers(num_tasks);
    for (int i = 0; i < num_tasks; i++) {
        int32_t task_annotations_buf_size = 0;
        void *task_annotations_buffer = nullptr;
        task_annotations_buffer = tasks[i]->task_annotations.pack(&task_annotations_buf_size);
        total_size += sizeof(int32_t) + task_annotations_buf_size; // size information + buffer size
        
        annotation_sizes[i] = task_annotations_buf_size;
        annotation_buffers[i] = task_annotations_buffer;
    }
    #endif

    #if CHAMELEON_TOOL_SUPPORT
    std::vector<int32_t> tool_data_buffer_sizes;
    std::vector<void*> tool_data_buffers;
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_encode_task_tool_data && cham_t_status.cham_t_callback_decode_task_tool_data) {
        tool_data_buffer_sizes.resize(num_tasks);
        tool_data_buffers.resize(num_tasks);

        for (int i = 0; i < num_tasks; i++) {
            int32_t task_tool_buf_size = 0;
            void *task_tool_buffer = nullptr;
            task_tool_buffer = cham_t_status.cham_t_callback_encode_task_tool_data(tasks[i], &(tasks[i]->task_tool_data), &task_tool_buf_size);
            total_size += sizeof(int32_t) + task_tool_buf_size; // size information + buffer size
            
            tool_data_buffer_sizes[i] = task_tool_buf_size;
            tool_data_buffers[i] = task_tool_buffer;
        }
    }
    #endif

    // allocate memory for transfer
    char *buff = (char *) malloc(total_size);
    char *cur_ptr = (char *)buff;

    // number of tasks in this message
    ((int32_t *) cur_ptr)[0] = num_tasks;
    cur_ptr += sizeof(int32_t);

    for (int i = 0; i < num_tasks; i++) {

        // 1. target entry address
        ((intptr_t *) cur_ptr)[0] = (intptr_t) tasks[i]->tgt_entry_ptr;
        cur_ptr += sizeof(intptr_t);

        // 2. img index
        ((int32_t *) cur_ptr)[0] = tasks[i]->idx_image;
        cur_ptr += sizeof(int32_t);

        // 3. offset
        ((ptrdiff_t *) cur_ptr)[0] = tasks[i]->entry_image_offset;
        cur_ptr += sizeof(ptrdiff_t);

        // 4. task id
        ((TYPE_TASK_ID *) cur_ptr)[0] = tasks[i]->task_id;
        cur_ptr += sizeof(TYPE_TASK_ID);

        // 5. number of arguments
        ((int32_t *) cur_ptr)[0] = tasks[i]->arg_num;
        cur_ptr += sizeof(int32_t);

        // 6. argument sizes
        memcpy(cur_ptr, &(tasks[i]->arg_sizes[0]), tasks[i]->arg_num * sizeof(int64_t));
        cur_ptr += tasks[i]->arg_num * sizeof(int64_t);

        // 7. offsets
        memcpy(cur_ptr, &(tasks[i]->arg_tgt_offsets[0]), tasks[i]->arg_num * sizeof(ptrdiff_t));
        cur_ptr += tasks[i]->arg_num * sizeof(ptrdiff_t);

        // 8. argument types
        memcpy(cur_ptr, &(tasks[i]->arg_types[0]), tasks[i]->arg_num * sizeof(int64_t));
        cur_ptr += tasks[i]->arg_num * sizeof(int64_t);

        #if OFFLOAD_DATA_PACKING_TYPE == 0
        // 9. loop through arguments and copy values
        for(int32_t i_arg = 0; i_arg < tasks[i]->arg_num; i_arg++) {
            int is_lit      = tasks[i]->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_from     = tasks[i]->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_FROM;

            print_arg_info("encode_send_buffer", tasks[i], i_arg);

            // copy value from host pointer directly
            if(is_lit) {
                ((intptr_t *) cur_ptr)[0] = (intptr_t) tasks[i]->arg_hst_pointers[i_arg];
            } else {
                memcpy(cur_ptr, (char*)tasks[i]->arg_hst_pointers[i_arg], tasks[i]->arg_sizes[i_arg]);
            }
            cur_ptr += tasks[i]->arg_sizes[i_arg];
        }
        #endif /* OFFLOAD_DATA_PACKING_TYPE */

        #if CHAM_MIGRATE_ANNOTATIONS
        int32_t task_annotations_buf_size = annotation_sizes[i];
        void * task_annotations_buffer = annotation_buffers[i];

        ((int32_t *) cur_ptr)[0] = task_annotations_buf_size;
        cur_ptr += sizeof(int32_t);

        if(task_annotations_buf_size > 0) {
            memcpy(cur_ptr, task_annotations_buffer, task_annotations_buf_size);
            cur_ptr += task_annotations_buf_size;
            // clean up again
            free(task_annotations_buffer);
        }
        #endif /* CHAM_MIGRATE_ANNOTATIONS */

        #if CHAMELEON_TOOL_SUPPORT
        if(cham_t_status.enabled && cham_t_status.cham_t_callback_encode_task_tool_data && cham_t_status.cham_t_callback_decode_task_tool_data) {
            int32_t task_tool_buf_size  = tool_data_buffer_sizes[i];
            void * task_tool_buffer     = tool_data_buffers[i];

            // remember size of buffer
            ((int32_t *) cur_ptr)[0] = task_tool_buf_size;
            cur_ptr += sizeof(int32_t);

            if(task_tool_buf_size) {
                memcpy(cur_ptr, task_tool_buffer, task_tool_buf_size);
                cur_ptr += task_tool_buf_size;
                // clean up again
                free(task_tool_buffer);
            }
        }
        #endif /* CHAMELEON_TOOL_SUPPORT */
    }

    // set output size
    *buffer_size = total_size;
#ifdef TRACE
    VT_END_W_CONSTRAINED(event_encode);
#endif
    return buff;
}

void decode_send_buffer(void * buffer, int mpi_tag, int32_t *num_tasks, std::vector<cham_migratable_task_t*> &tasks) {
#ifdef TRACE
    static int event_decode = -1;
    std::string event_decode_name = "decode";
    if(event_decode == -1) 
        int ierr = VT_funcdef(event_decode_name.c_str(), VT_NOCLASS, &event_decode);
    VT_BEGIN_CONSTRAINED(event_decode);
#endif
    // current pointer position
    char *cur_ptr = (char*) buffer;

    // get number of tasks
    int n_tasks = ((int32_t *) cur_ptr)[0];
    cur_ptr += sizeof(int32_t);

    DBP("decode_send_buffer (enter) - mpi_tag: %d, num_tasks: %d\n", mpi_tag, n_tasks);

    *num_tasks = n_tasks;
    tasks.resize(n_tasks);

    // init new tasks and make sure that each is freeable again
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        cham_migratable_task_t* task    = new cham_migratable_task_t();
        task->is_remote_task            = 1;

        // 1. target function pointer
        task->tgt_entry_ptr = ((intptr_t *) cur_ptr)[0];
        cur_ptr += sizeof(intptr_t);

        // 2. img index
        task->idx_image = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);

        // 3. offset
        task->entry_image_offset = ((ptrdiff_t *) cur_ptr)[0];
        cur_ptr += sizeof(ptrdiff_t);

        // 4. task id
        task->task_id = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);

        // 5. number of arguments
        task->arg_num = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);

        DBP("decode_send_buffer - task_entry (task_id=%ld): " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", task->task_id, DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

        // we need a mapping to process local task entry points
        intptr_t local_img_base     = _image_base_addresses[task->idx_image];
        intptr_t local_entry        = local_img_base + task->entry_image_offset;
        DBP("decode_send_buffer - mapping remote entry point from: " DPxMOD " to local: " DPxMOD "\n", DPxPTR(task->tgt_entry_ptr), DPxPTR(local_entry));
        task->tgt_entry_ptr         = local_entry;

        // resize data structure
        task->ReSizeArrays(task->arg_num);
        
        // 6. argument sizes
        memcpy(&(task->arg_sizes[0]), cur_ptr, task->arg_num * sizeof(int64_t));
        cur_ptr += task->arg_num * sizeof(int64_t);

        // 7. argument types
        memcpy(&(task->arg_tgt_offsets[0]), cur_ptr, task->arg_num * sizeof(ptrdiff_t));
        cur_ptr += task->arg_num * sizeof(ptrdiff_t);

        // 8. offsets
        memcpy(&(task->arg_types[0]), cur_ptr, task->arg_num * sizeof(int64_t));
        cur_ptr += task->arg_num * sizeof(int64_t);

        // 9. loop through arguments and copy values
        for(int32_t i_arg = 0; i_arg < task->arg_num; i_arg++) {
            int is_lit      = task->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            // int is_from     = task->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_FROM;

            #if OFFLOAD_DATA_PACKING_TYPE == 0
            // copy value from host pointer directly
            if(is_lit) {
                intptr_t tmp_ptr = ((intptr_t *) cur_ptr)[0];
                task->arg_hst_pointers[i_arg] = (void *) tmp_ptr;
            } else {
                // need to allocate new memory
                void * new_mem = malloc(task->arg_sizes[i_arg]);
                memcpy(new_mem, cur_ptr, task->arg_sizes[i_arg]);
                task->arg_hst_pointers[i_arg] = new_mem;
            }
            // increment pointer
            cur_ptr += task->arg_sizes[i_arg];
            #elif OFFLOAD_DATA_PACKING_TYPE > 0
            // allocate memory for host pointer
            if(!is_lit) {
                void * new_mem = malloc(task->arg_sizes[i_arg]);
                task->arg_hst_pointers[i_arg] = new_mem;
            }
            #endif
            print_arg_info("decode_send_buffer", task, i_arg);
        }

        #if CHAM_MIGRATE_ANNOTATIONS
        // task annotations
        int32_t task_annotations_buf_size = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);
        if(task_annotations_buf_size > 0) {
            task->task_annotations.unpack((void*)cur_ptr);
            cur_ptr += task_annotations_buf_size;
        }
        #endif

        #if CHAMELEON_TOOL_SUPPORT
        if(cham_t_status.enabled && cham_t_status.cham_t_callback_encode_task_tool_data && cham_t_status.cham_t_callback_decode_task_tool_data) {
            // first get size of buffer
            int32_t task_tool_buf_size = ((int32_t *) cur_ptr)[0];
            cur_ptr += sizeof(int32_t);

            if(task_tool_buf_size > 0) {
                cham_t_status.cham_t_callback_decode_task_tool_data(task, &(task->task_tool_data), (void*)cur_ptr, task_tool_buf_size);
                cur_ptr += task_tool_buf_size;
            }
        }
        #endif
        tasks[i_task] = task;
    }

#ifdef TRACE
    VT_END_W_CONSTRAINED(event_decode);
#endif
}
#pragma endregion Offloading / Packing

#pragma region Helper Functions
int exit_condition_met(int from_taskwait, int print) {
    if(from_taskwait) {
        int cp_ranks_not_completely_idle = _num_ranks_not_completely_idle.load();
        if( _comm_thread_load_exchange_happend && _outstanding_jobs_sum.load() == 0 && cp_ranks_not_completely_idle == 0) {
            // if(print)
                // DBP("exit_condition_met - exchange_happend: %d oustanding: %d _num_ranks_not_completely_idle: %d\n", 
                //     _comm_thread_load_exchange_happend.load(), 
                //     _outstanding_jobs_sum.load(), 
                //     cp_ranks_not_completely_idle);
            return 1;
        }
    } else {
        if( _num_threads_idle >= _num_threads_involved_in_taskwait) {
            int cp_ranks_not_completely_idle = _num_ranks_not_completely_idle.load();
            if( _comm_thread_load_exchange_happend && _outstanding_jobs_sum.load() == 0 && cp_ranks_not_completely_idle == 0) {
                // if(print)
                    // DBP("exit_condition_met - exchange_happend: %d oustanding: %d _num_ranks_not_completely_idle: %d\n", 
                    //     _comm_thread_load_exchange_happend.load(), 
                //     _comm_thread_load_exchange_happend.load(), 
                    //     _comm_thread_load_exchange_happend.load(), 
                    //     _outstanding_jobs_sum.load(), 
                //     _outstanding_jobs_sum.load(), 
                    //     _outstanding_jobs_sum.load(), 
                    //     cp_ranks_not_completely_idle);
                return 1;
            }
        }
    }    
    return 0;
}

void trigger_update_outstanding() {
    _outstanding_jobs_local     = _num_local_tasks_outstanding.load() + _num_stolen_tasks_outstanding.load();
}

void print_arg_info(std::string prefix, cham_migratable_task_t *task, int idx) {
    int64_t tmp_type    = task->arg_types[idx];
    int is_lit          = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;
    int is_from         = tmp_type & CHAM_OMP_TGT_MAPTYPE_FROM;

    DBP("%s - arg: " DPxMOD ", size: %ld, type: %ld, offset: %ld, literal: %d, from: %d\n", 
            prefix.c_str(),
            DPxPTR(task->arg_hst_pointers[idx]), 
            task->arg_sizes[idx],
            task->arg_types[idx],
            task->arg_tgt_offsets[idx],
            is_lit,
            is_from);
}

void print_arg_info_w_tgt(std::string prefix, cham_migratable_task_t *task, int idx) {
    int64_t tmp_type    = task->arg_types[idx];
    int is_lit          = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;
    int is_from         = tmp_type & CHAM_OMP_TGT_MAPTYPE_FROM;

    DBP("%s - arg_tgt: " DPxMOD ", arg_hst: " DPxMOD ", size: %ld, type: %ld, offset: %ld, literal: %d, from: %d\n", 
            prefix.c_str(),
            DPxPTR(task->arg_tgt_pointers[idx]), 
            DPxPTR(task->arg_hst_pointers[idx]), 
            task->arg_sizes[idx],
            task->arg_types[idx],
            task->arg_tgt_offsets[idx],
            is_lit,
            is_from);
}
#pragma endregion Helper Functions

#pragma region CommThread
inline void action_create_gather_request(int *num_threads_in_tw, int *transported_load_values, int* buffer_load_values, MPI_Request *request_gather_out) {
    int32_t local_load_representation;
    int32_t num_tasks_local = _local_tasks.dup_size();
    TYPE_TASK_ID* ids_local = nullptr;
    int32_t num_tasks_stolen = _stolen_remote_tasks.dup_size();
    TYPE_TASK_ID* ids_stolen = nullptr;
    
    #if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_determine_local_load) {
        // only get task ids when tool is used since default mode does not require that information
        ids_local   = _local_tasks.get_task_ids(&num_tasks_local);
        ids_stolen  = _stolen_remote_tasks.get_task_ids(&num_tasks_stolen);
        local_load_representation = cham_t_status.cham_t_callback_determine_local_load(ids_local, num_tasks_local, ids_stolen, num_tasks_stolen);
        // clean up again
        free(ids_local);
        free(ids_stolen);
    } else {
        local_load_representation = getDefaultLoadInformationForRank(ids_local, num_tasks_local, ids_stolen, num_tasks_stolen);
    }
    #else 
    local_load_representation = getDefaultLoadInformationForRank(ids_local, num_tasks_local, ids_stolen, num_tasks_stolen);
    #endif

    int tmp_val = _num_threads_idle.load() < *num_threads_in_tw ? 1 : 0;
    // DBP("action_create_gather_request - my current value for rank_not_completely_in_taskwait: %d\n", tmp_val);
    transported_load_values[0] = tmp_val;
    _mtx_load_exchange.lock();
    transported_load_values[1] = _outstanding_jobs_local.load();
    _mtx_load_exchange.unlock();
    transported_load_values[2] = local_load_representation;
    
    MPI_Iallgather(transported_load_values, 3, MPI_INT, buffer_load_values, 3, MPI_INT, chameleon_comm_load, request_gather_out);
}

inline void action_handle_gather_request(int *event_exchange_outstanding, int *buffer_load_values, int *request_gather_created, int *last_known_sum_outstanding, int *offload_triggered) {
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(*event_exchange_outstanding);
    #endif
    // sum up that stuff
    // DBP("action_handle_gather_request - gathered new load info\n");
    int32_t old_val_n_ranks                 = _num_ranks_not_completely_idle;
    int32_t old_outstanding_sum             = _outstanding_jobs_sum.load();
    int32_t sum_ranks_not_completely_idle   = 0;
    int32_t sum_outstanding                 = 0;

    for(int j = 0; j < chameleon_comm_size; j++) {
        // DBP("action_handle_gather_request - values for rank %d: all_in_tw=%d, outstanding_jobs=%d, load=%d\n", j, buffer_load_values[j*2], buffer_load_values[(j*2)+1], buffer_load_values[(j*2)+2]);
        int tmp_tw                              = buffer_load_values[j*3];
        _outstanding_jobs_ranks[j]              = buffer_load_values[(j*3)+1];
        _load_info_ranks[j]                     = buffer_load_values[(j*3)+2];
        sum_outstanding                         += _outstanding_jobs_ranks[j];
        sum_ranks_not_completely_idle           += tmp_tw;
        // DBP("load info from rank %d = %d\n", j, _load_info_ranks[j]);
    }
    _num_ranks_not_completely_idle      = sum_ranks_not_completely_idle;
    // if(old_val_n_ranks != sum_ranks_not_completely_idle || old_outstanding_sum != sum_outstanding) {
    //     DBP("action_handle_gather_request - _num_ranks_not_completely_idle: old=%d new=%d\n", old_val_n_ranks, sum_ranks_not_completely_idle);
    //     DBP("action_handle_gather_request - _outstanding_jobs_sum: old=%d new=%d\n", old_outstanding_sum, sum_outstanding);
    // }
    _outstanding_jobs_sum               = sum_outstanding;

    // reset flag
    *request_gather_created = 0;
    
    #if OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED
    if(*last_known_sum_outstanding == -1) {
        *last_known_sum_outstanding = sum_outstanding;
        *offload_triggered = 0;
        // DBP("action_handle_gather_request - sum outstanding operations=%d, nr_open_requests_send=%d\n", *last_known_sum_outstanding, request_manager_send.getNumberOfOutstandingRequests());
    } else {
        // check whether changed.. only allow new offload after change
        if(*last_known_sum_outstanding != sum_outstanding) {
            *last_known_sum_outstanding = sum_outstanding;
            *offload_triggered = 0;
            // DBP("action_handle_gather_request - sum outstanding operations=%d, nr_open_requests_send=%d\n", *last_known_sum_outstanding, request_manager_send.getNumberOfOutstandingRequests());
        }
    }
    #else
    *offload_triggered = 0;
    #endif /* OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED */

    #ifdef TRACE
    VT_END_W_CONSTRAINED(*event_exchange_outstanding);
    #endif
}

inline void action_task_migration(int *event_offload_decision, int *offload_triggered, int *num_threads_in_tw, std::vector<int32_t> &tasksToOffload) {
    // only check for offloading if enough local tasks available and exchange has happend at least once
    #if FORCE_MIGRATION
    if(_comm_thread_load_exchange_happend && *offload_triggered == 0) {
    #else
    // also only proceed if offloading not already performed.. wait for new load exchange
    if(_comm_thread_load_exchange_happend && _local_tasks.dup_size() >= MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION && *offload_triggered == 0) {
    #endif

        #if OFFLOAD_BLOCKING
        if(!_offload_blocked) {
        #endif

        // Strategies for speculative load exchange
        // - If we find a rank with load = 0 ==> offload directly
        // - Should we look for the minimum? Might be a critical part of the program because several ranks might offload to that rank
        // - Just offload if there is a certain difference between min and max load to avoid unnecessary offloads
        // - Sorted approach: rank with highest load should offload to rank with minimal load
        // - Be careful about balance between computational complexity of calculating the offload target and performance gain that can be achieved
        
            #ifdef TRACE
            VT_BEGIN_CONSTRAINED(*event_offload_decision);
            #endif

            int strategy_type;

            // reset values to zero
            std::fill(tasksToOffload.begin(), tasksToOffload.end(), 0);
            int32_t num_tasks_local = 0;
            TYPE_TASK_ID* ids_local;
            cham_t_migration_tupel_t* migration_tupels = nullptr;
            int32_t num_tuples = 0;
            
            // use different atomic for that to avoid to much contention (maybe _num_open_tasks_stolen)
            // oustanding includes tasks that are currently in execution whereas open means tasks in queue only
            int32_t num_tasks_stolen = _stolen_remote_tasks.dup_size();
            
            #if CHAMELEON_TOOL_SUPPORT && !FORCE_MIGRATION
            if(cham_t_status.enabled && cham_t_status.cham_t_callback_select_tasks_for_migration) {
                strategy_type = 1;
                ids_local = _local_tasks.get_task_ids(&num_tasks_local);
                migration_tupels = cham_t_status.cham_t_callback_select_tasks_for_migration(&(_load_info_ranks[0]), ids_local, num_tasks_local, num_tasks_stolen, &num_tuples);
                free(ids_local);
            } else if(cham_t_status.enabled && cham_t_status.cham_t_callback_select_num_tasks_to_offload) {
                strategy_type = 0;
                num_tasks_local     = _local_tasks.dup_size();
                cham_t_status.cham_t_callback_select_num_tasks_to_offload(&(tasksToOffload[0]), &(_load_info_ranks[0]), num_tasks_local, num_tasks_stolen);
            } else {
                strategy_type = 0;
                num_tasks_local     = _local_tasks.dup_size();
                computeNumTasksToOffload( tasksToOffload, _load_info_ranks, num_tasks_local, num_tasks_stolen);
            }
            #else
            strategy_type = 0;
            num_tasks_local     = _local_tasks.dup_size();
            computeNumTasksToOffload( tasksToOffload, _load_info_ranks, num_tasks_local, num_tasks_stolen);
            #endif

            #if CHAM_STATS_RECORD
            _num_migration_decision_performed++;
            #endif

            #ifdef TRACE
            VT_END_W_CONSTRAINED(*event_offload_decision);
            #endif

            bool offload_done = false;
            double my_current_load = (double) _load_info_ranks[chameleon_comm_rank];

            if(strategy_type == 1)
            {
                // strategy type that uses tupels of task_id and target rank
                if(migration_tupels) {

                    // temporary maps to collect tasks per target rank
                    std::unordered_map<int, std::vector<cham_migratable_task_t*>> map_task_vec;
                    std::unordered_map<int, int> map_task_num;

                    for(int32_t t = 0; t < num_tuples; t++) {
                        TYPE_TASK_ID cur_task_id    = migration_tupels[t].task_id;
                        int cur_rank_id             = migration_tupels[t].rank_id;

                        // get task by id
                        cham_migratable_task_t* task = _local_tasks.pop_task_by_id(cur_task_id);
                        if(task) {
                            
                            // check whether vector for rank already existing
                            std::vector<cham_migratable_task_t*> cur_map;
                            std::unordered_map<int, std::vector<cham_migratable_task_t*>>::const_iterator got = map_task_vec.find(cur_rank_id);
                            if(got != map_task_vec.end()) {
                                cur_map = got->second;
                            } else {
                                // cur_map = std::vector<cham_migratable_task_t*>();
                                map_task_vec.insert(std::make_pair(cur_rank_id, cur_map));
                                map_task_num.insert(std::make_pair(cur_rank_id, 0));
                            }

                            // add task to vector
                            if(map_task_num[cur_rank_id] < MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE) {
                                map_task_num[cur_rank_id]++;
                                map_task_vec[cur_rank_id].push_back(task);
                            }

                            offload_done = true;

                            #if OFFLOAD_BLOCKING
                            _offload_blocked = 1;
                            #endif
                        }
                    }

                    std::vector<int> keys;
                    keys.reserve(map_task_num.size());

                    for(auto kv : map_task_num) {
                        keys.push_back(kv.first);
                    }

                    for (auto &r_id : keys) {
                        std::vector<cham_migratable_task_t*> cur_tasks = map_task_vec[r_id];
                        int num_tasks = map_task_num[r_id];

                        #if OFFLOAD_SEND_TASKS_SEPARATELY
                        for(int i_task = 0; i_task < num_tasks; i_task++) {
                            offload_tasks_to_rank(&cur_tasks[i_task], 1, r_id);
                        }
                        #else
                        offload_tasks_to_rank(&cur_tasks[0], num_tasks, r_id);
                        #endif
                    }

                    // cleanup tupels again
                    free(migration_tupels);
                }
            } else {
                for(int r=0; r<tasksToOffload.size(); r++) {
                    if(r != chameleon_comm_rank) {
                        int num_tasks_to_migrate = tasksToOffload[r];
                        
                        int num_tasks = 0;
                        std::vector<cham_migratable_task_t*> cur_tasks(num_tasks_to_migrate);

                        for(int t=0; t < num_tasks_to_migrate; t++) {
                            cham_migratable_task_t *task = _local_tasks.pop_front();
                            if(task) {
                                cur_tasks[num_tasks] = task;
                                num_tasks++;

                                // stop when limit reached
                                if(num_tasks >= MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE) {
                                    // RELP("num_tasks:%d, MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE:%f\n", num_tasks, MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE.load())
                                    break;
                                }
                            }
                        }

                        if(num_tasks > 0) {
                            #if OFFLOAD_SEND_TASKS_SEPARATELY
                            for(int i_task = 0; i_task < num_tasks; i_task++) {
                                offload_tasks_to_rank(&cur_tasks[i_task], 1, r);
                            }
                            #else
                            // double victim_load  = (double) _load_info_ranks[r];
                            // double cur_diff     = (my_current_load-victim_load);
                            // double cur_ratio    = cur_diff / victim_load;
                            // RELP("Migrating\t%d\ttasks to rank:\t%d\tload:\t%f\tload_victim:\t%f\tratio:\t%f\tdiff:\t%f\n", num_tasks, r, my_current_load, victim_load, cur_ratio, cur_diff);
                            offload_tasks_to_rank(&cur_tasks[0], num_tasks, r);
                            #endif

                            offload_done = true;
                            #if OFFLOAD_BLOCKING
                            _offload_blocked = 1;
                            #endif
                        }
                    }
                }
            }
            if(offload_done) {
                *offload_triggered = 1;
                #if CHAM_STATS_RECORD
                _num_migration_done++;
                #endif /* CHAM_STATS_RECORD */    
            }
        #if OFFLOAD_BLOCKING
        }
        #endif
    }
}

inline void action_send_back_stolen_tasks(int *event_send_back, cham_migratable_task_t *cur_task, RequestManager *request_manager_send) {
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(*event_send_back);
    #endif

    DBP("send_back_stolen_tasks - sending back data to rank %d with tag %d for (task_id=%ld)\n", cur_task->source_mpi_rank, cur_task->task_id, cur_task->task_id);

    #if OFFLOAD_DATA_PACKING_TYPE == 0
    #if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
    #endif

    int32_t tmp_size_buff = 0;
    for(int i = 0; i < cur_task->arg_num; i++) {
        if(cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
            tmp_size_buff += cur_task->arg_sizes[i];
        }
    }
    // allocate memory
    void * buff = malloc(tmp_size_buff);
    char* cur_ptr = (char*)buff;
    for(int i = 0; i < cur_task->arg_num; i++) {
        if(cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
            print_arg_info("send_back_stolen_tasks", cur_task, i);
            memcpy(cur_ptr, cur_task->arg_hst_pointers[i], cur_task->arg_sizes[i]);
            cur_ptr += cur_task->arg_sizes[i];
        }
    }
    MPI_Request request;
    MPI_Isend(buff, tmp_size_buff, MPI_BYTE, cur_task->source_mpi_rank, cur_task->task_id, chameleon_comm_mapped, &request);
    
    #if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_comm_back_send_sum, cur_time);
    _time_comm_back_send_count++;
    #endif

    request_manager_send->submitRequests( cur_task->task_id, cur_task->source_mpi_rank, 1, &request, MPI_BLOCKING, send_back_handler, sendBack, buff, &cur_task, 1);
    
    #elif OFFLOAD_DATA_PACKING_TYPE > 0
    #if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
    #endif

    #if OFFLOAD_DATA_PACKING_TYPE == 1
    MPI_Request *requests = new MPI_Request[cur_task->arg_num];
    int num_requests = 0;
    for(int i = 0; i < cur_task->arg_num; i++) {
        if(cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
            MPI_Isend(cur_task->arg_hst_pointers[i], cur_task->arg_sizes[i], MPI_BYTE, cur_task->source_mpi_rank, cur_task->task_id, chameleon_comm_mapped, &requests[num_requests++]);
        }
    }
    #elif OFFLOAD_DATA_PACKING_TYPE == 2
    int num_requests = 1;
    MPI_Request *requests = new MPI_Request[num_requests];
    MPI_Datatype type_mapped_vars;
    int num_outputs = 0;
    for(int i=0; i< cur_task->arg_num; i++) {
        if(cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
            num_outputs++;
        }
    }
    MPI_Datatype separate_types[num_outputs];
    int blocklen[num_outputs];
    MPI_Aint disp[num_outputs];
    int ierr = 0;
    int j = 0;
    for(int i=0; i < cur_task->arg_num; i++) {
        int is_from         = cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;            
        if(is_from) {
            separate_types[j]   = MPI_BYTE;
            blocklen[j]         = cur_task->arg_sizes[i];
            int is_lit          = cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            if(is_lit) {
                ierr = MPI_Get_address(&cur_task->arg_hst_pointers[i], &(disp[j]));
                assert(ierr==MPI_SUCCESS);
            }
            else {
                ierr = MPI_Get_address(cur_task->arg_hst_pointers[i], &(disp[j]));
                assert(ierr==MPI_SUCCESS);
            }
            j++;
        }
    }
    ierr = MPI_Type_create_struct(num_outputs, blocklen, disp, separate_types, &type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_commit(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Isend(MPI_BOTTOM, 1, type_mapped_vars, cur_task->source_mpi_rank, cur_task->task_id, chameleon_comm_mapped, &requests[0]);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_free(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    #endif

    #if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_comm_back_send_sum, cur_time);
    _time_comm_back_send_count++;
    #endif

    cham_migratable_task_t **tasks = (cham_migratable_task_t **) malloc(sizeof(cham_migratable_task_t *));
    tasks[0] = cur_task;
    request_manager_send->submitRequests( cur_task->task_id, cur_task->source_mpi_rank, num_requests, &requests[0], MPI_BLOCKING, send_back_handler, sendBack, nullptr, tasks, 1);
    delete[] requests;
    #endif /* OFFLOAD_DATA_PACKING_TYPE */

    _mtx_load_exchange.lock();
    _num_stolen_tasks_outstanding--;
    DBP("send_back_stolen_tasks - decrement stolen outstanding count for task %ld\n", cur_task->task_id);
    trigger_update_outstanding();
    _mtx_load_exchange.unlock();

    #ifdef TRACE
    VT_END_W_CONSTRAINED(*event_send_back);
    #endif
}

inline void action_handle_cancel_request(MPI_Status *cur_status_cancel) {
    TYPE_TASK_ID task_id = -1;
    MPI_Recv(&task_id, 1, MPI_INTEGER, cur_status_cancel->MPI_SOURCE, 0, chameleon_comm_cancel, MPI_STATUS_IGNORE);
    DBP("action_handle_cancel_request - received cancel request for task_id %ld\n", task_id);

    cham_migratable_task_t *task = _map_tag_to_stolen_task.find(task_id);            
    if(task) {
        bool expected = false;
        bool desired = true;

        if(task->sync_commthread_lock.compare_exchange_strong(expected, desired)) {
            DBP("receive_remote_tasks - cancelling task with task_id %ld\n", task_id);
            _stolen_remote_tasks.remove(task);
            _map_tag_to_stolen_task.erase(task->task_id);
            _map_overall_tasks.erase(task->task_id);

            // decrement load counter and ignore send back
            _mtx_load_exchange.lock();
            _num_stolen_tasks_outstanding--;
            DBP("receive_remote_tasks(cancel) - decrement stolen outstanding count for task %ld\n", task->task_id);
            trigger_update_outstanding();
            _mtx_load_exchange.unlock();

            #if CHAM_STATS_RECORD
            _num_tasks_canceled++;
            #endif       
        } else {}//do nothing -> task either has been executed or is currently executed, process_remote_task will take care of cleaning up
    }
}

inline void action_handle_recvback_request(MPI_Status *cur_status_receiveBack, RequestManager *request_manager_receive, int *event_recv_back) {
    cham_migratable_task_t *task_entry = _map_offloaded_tasks_with_outputs.find(cur_status_receiveBack->MPI_TAG);
    if(task_entry) {
        #if CHAM_STATS_RECORD
        double cur_time;
        #endif

        DBP("action_handle_recvback_request - receiving back task with id %ld\n", task_entry->task_id);
        // check if we still need to receive the task data back or replicated task is executed locally already
        bool expected = false;
        bool desired = true;
        // DBP("action_handle_recvback_request - performing CAS for task with id %ld, flag %d\n", task_entry->task_id, task_entry->sync_commthread_lock.load());
        //assert(task_entry->sync_commthread_lock.load()==false);
        bool exchanged = task_entry->sync_commthread_lock.compare_exchange_strong(expected, desired);
        //assert(exchanged);
        // DBP("action_handle_recvback_request - CAS: expected = %d, desired = %d, exchanged = %d\n", expected, desired, exchanged);
        //atomic CAS   
        if(exchanged) {
            DBP("action_handle_recvback_request - posting receive requests for task with id %ld\n", task_entry->task_id);
            //remove from replicated task queue -> corresponds to local task cancellation                   
            _replicated_tasks.remove(task_entry);

            //we can safely receive back as usual
            
            #if OFFLOAD_DATA_PACKING_TYPE == 0
            //entry is removed in receiveBackHandler!
            // receive data back
            int recv_buff_size;
            MPI_Get_count(cur_status_receiveBack, MPI_BYTE, &recv_buff_size);
            void * buffer = malloc(recv_buff_size);
            
            #ifdef TRACE
            VT_BEGIN_CONSTRAINED(*event_recv_back);
            #endif

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime();
            #endif

            MPI_Request request;
            MPI_Irecv(buffer, recv_buff_size, MPI_BYTE, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG,
                                                                                chameleon_comm_mapped, &request);
            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            atomic_add_dbl(_time_comm_back_recv_sum, cur_time);
            _time_comm_back_recv_count++;
            #endif

            request_manager_receive->submitRequests( cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, 1, 
                                                    &request,
                                                    MPI_BLOCKING,
                                                    receive_back_handler,
                                                    recvBack,
                                                    buffer);

            #ifdef TRACE
            VT_END_W_CONSTRAINED(*event_recv_back);
            #endif

            #elif OFFLOAD_DATA_PACKING_TYPE == 1

            #ifdef TRACE
            VT_BEGIN_CONSTRAINED(*event_recv_back);
            #endif

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime();
            #endif

            MPI_Request *requests = new MPI_Request[task_entry->arg_num];  
            int j = 0;
            for(int i = 0; i < task_entry->arg_num; i++) {
                if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                    MPI_Irecv(task_entry->arg_hst_pointers[i], task_entry->arg_sizes[i], MPI_BYTE, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG,
                                                                                chameleon_comm_mapped, &requests[j++]);
                }
            }

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            atomic_add_dbl(_time_comm_back_recv_sum, cur_time);
            _time_comm_back_recv_count++;
            #endif
            
            request_manager_receive->submitRequests( cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, j, 
                                                &requests[0],
                                                MPI_BLOCKING,
                                                receive_back_handler,
                                                recvBack,
                                                nullptr);
            delete[] requests;
            
            #ifdef TRACE
            VT_END_W_CONSTRAINED(*event_recv_back);
            #endif

            #elif OFFLOAD_DATA_PACKING_TYPE == 2

            #ifdef TRACE
            VT_BEGIN_CONSTRAINED(*event_recv_back);
            #endif

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime();
            #endif

            MPI_Request *requests = new MPI_Request[1];
            MPI_Datatype type_mapped_vars;
            int num_outputs = 0;
            for(int i=0; i< task_entry->arg_num; i++) {
                if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                    num_outputs++;
                }
            }
            MPI_Datatype separate_types[num_outputs];
            int blocklen[num_outputs];
            MPI_Aint disp[num_outputs];
            int ierr = 0;
            int j = 0;
            for(int i=0; i < task_entry->arg_num; i++) {
                int is_from = task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;            
                if(is_from) {
                    separate_types[j]   = MPI_BYTE;
                    blocklen[j]         = task_entry->arg_sizes[i];
                    int is_lit          = task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
                    if(is_lit) {
                        ierr = MPI_Get_address(&task_entry->arg_hst_pointers[i], &(disp[j]));
                        assert(ierr==MPI_SUCCESS);
                    }
                    else {
                        ierr = MPI_Get_address(task_entry->arg_hst_pointers[i], &(disp[j]));
                        assert(ierr==MPI_SUCCESS);
                    }
                    j++;
                }
            }
            ierr = MPI_Type_create_struct(num_outputs, blocklen, disp, separate_types, &type_mapped_vars);
            assert(ierr==MPI_SUCCESS);
            ierr = MPI_Type_commit(&type_mapped_vars);
            assert(ierr==MPI_SUCCESS);
            ierr = MPI_Irecv(MPI_BOTTOM, 1, type_mapped_vars, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG, chameleon_comm_mapped, &requests[0]);
            assert(ierr==MPI_SUCCESS);
            ierr = MPI_Type_free(&type_mapped_vars);
            assert(ierr==MPI_SUCCESS);

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            atomic_add_dbl(_time_comm_back_recv_sum, cur_time);
            _time_comm_back_recv_count++;
            #endif

            request_manager_receive->submitRequests( cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, 1, 
                                                &requests[0],
                                                MPI_BLOCKING,
                                                receive_back_handler,
                                                recvBack,
                                                nullptr);
            delete[] requests;

            #ifdef TRACE 
            VT_END_W_CONSTRAINED(*event_recv_back);
            #endif
            #endif /* OFFLOAD_DATA_PACKING_TYPE */
        }  //CAS
        else // CAS didn't succeed -> we need to receive data into trash buffer
        {
            DBP("Late receive back occured for replicated task, task_id %ld\n", task_entry->task_id);
            #if OFFLOAD_DATA_PACKING_TYPE == 0
            int msg_size = 0;
            MPI_Get_count(&cur_status_receiveBack, MPI_BYTE, &msg_size);
            if(msg_size > cur_trash_buffer_size) {
                free(trash_buffer);
                trash_buffer = malloc(msg_size);
                cur_trash_buffer_size = msg_size; 
            }     
            MPI_Request request;

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime();
            #endif

            MPI_Irecv(trash_buffer, msg_size, MPI_BYTE, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG, chameleon_comm_mapped, &request);

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            atomic_add_dbl(_time_comm_back_recv_sum, cur_time);
            _time_comm_back_recv_count++; //TODO count trash receives!
            #endif

            request_manager_receive->submitRequests( cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, 1, 
                                                    &request,
                                                    MPI_BLOCKING,
                                                    receive_back_trash_handler,
                                                    recvBack,
                                                    buffer);

            #elif OFFLOAD_DATA_PACKING_TYPE > 0 // TODO: need to take care of Type 2
            
            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime();
            #endif   

            MPI_Request *requests = new MPI_Request[task_entry->arg_num];
            int j = 0;
            for(int i = 0; i < task_entry->arg_num; i++) {
                if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                    if(task_entry->arg_sizes[i]> cur_trash_buffer_size) {
                        free(trash_buffer);
                        trash_buffer = malloc(task_entry->arg_sizes[i]);
                        cur_trash_buffer_size = task_entry->arg_sizes[i];
                    }                         
                    MPI_Irecv(trash_buffer, task_entry->arg_sizes[i], MPI_BYTE, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG,
                                                                                chameleon_comm_mapped, &requests[j++]);
                }
            }

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            atomic_add_dbl(_time_comm_back_recv_sum, cur_time);
            _time_comm_back_recv_count++;
            #endif
            
            request_manager_receive->submitRequests( cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, j, 
                                                &requests[0],
                                                MPI_BLOCKING,
                                                receive_back_trash_handler,
                                                recvBack,
                                                nullptr);
            delete[] requests;
            #endif /* OFFLOAD_DATA_PACKING_TYPE */
        }
    } //match
}

inline void action_handle_recv_request(int *event_receive_tasks, MPI_Status *cur_status_receive, RequestManager *request_manager_receive) {
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(*event_receive_tasks);
    #endif

    DBP("Incoming receive request for task id %d from rank %d\n", cur_status_receive->MPI_TAG, cur_status_receive->MPI_SOURCE);
    
    int recv_buff_size = 0;
    MPI_Get_count(cur_status_receive, MPI_BYTE, &recv_buff_size);
    void *buffer = malloc(recv_buff_size);

    MPI_Request request = MPI_REQUEST_NULL;

    #if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
    #endif

    int res = MPI_Irecv(buffer, recv_buff_size, MPI_BYTE, cur_status_receive->MPI_SOURCE, cur_status_receive->MPI_TAG, chameleon_comm, &request);
    assert(res==MPI_SUCCESS);

    #if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_comm_recv_task_sum, cur_time);
    _time_comm_recv_task_count++;
    #endif

    request_manager_receive->submitRequests( cur_status_receive->MPI_TAG, 
                                    cur_status_receive->MPI_SOURCE,
                                    1, 
                                    &request,
                                    MPI_BLOCKING,
                                    receive_handler,
                                    recv,
                                    buffer);

    #ifdef TRACE
    VT_END_W_CONSTRAINED(*event_receive_tasks);
    #endif
}

void* comm_thread_action(void* arg) {
    pin_thread_to_last_core(1);
    // trigger signal to tell that thread is running now
    pthread_mutex_lock( &_th_service_actions_mutex );
    _th_service_actions_created = 1;
    pthread_cond_signal( &_th_service_actions_cond );
    pthread_mutex_unlock( &_th_service_actions_mutex );

    static int event_receive_tasks          = -1;
    static int event_recv_back              = -1;
    static int event_exchange_outstanding   = -1;
    static int event_offload_decision       = -1;
    static int event_send_back              = -1;
    static int event_progress_send          = -1;
    static int event_progress_recv          = -1;

    #ifdef TRACE    
    std::string event_receive_tasks_name = "receive_task";
    if(event_receive_tasks == -1) 
        int ierr = VT_funcdef(event_receive_tasks_name.c_str(), VT_NOCLASS, &event_receive_tasks);
    
    std::string event_recv_back_name = "receive_back";
    if(event_recv_back == -1) 
        int ierr = VT_funcdef(event_recv_back_name.c_str(), VT_NOCLASS, &event_recv_back);
    
    std::string exchange_outstanding_name = "exchange_outstanding";
    if(event_exchange_outstanding == -1)
        int ierr = VT_funcdef(exchange_outstanding_name.c_str(), VT_NOCLASS, &event_exchange_outstanding);    
    
    std::string event_offload_decision_name = "offload_decision";
    if(event_offload_decision == -1)
        int ierr = VT_funcdef(event_offload_decision_name.c_str(), VT_NOCLASS, &event_offload_decision);

    std::string event_send_back_name = "send_back";
    if(event_send_back == -1)
        int ierr = VT_funcdef(event_send_back_name.c_str(), VT_NOCLASS, &event_send_back);

    std::string event_progress_send_name = "progress_send";
    if(event_progress_send == -1) 
        int ierr = VT_funcdef(event_progress_send_name.c_str(), VT_NOCLASS, &event_progress_send);

    std::string event_progress_recv_name = "progress_recv";
    if(event_progress_recv == -1) 
        int ierr = VT_funcdef(event_progress_recv_name.c_str(), VT_NOCLASS, &event_progress_recv);
    #endif

    // =============== General Vars
    int err;
    int flag_set                    = 0;
    int num_threads_in_tw           = _num_threads_involved_in_taskwait.load();
    double cur_time;
    double time_last_load_exchange  = 0;
    double time_gather_posted       = 0;

    // =============== Send Thread Vars
    int request_gather_created      = 0;
    MPI_Request request_gather_out;
    MPI_Status  status_gather_out;
    int offload_triggered           = 0;
    int last_known_sum_outstanding  = -1;
    
    int transported_load_values[3];
    int * buffer_load_values        = (int*) malloc(sizeof(int)*3*chameleon_comm_size);
    std::vector<int32_t> tasksToOffload(chameleon_comm_size);

    // =============== Recv Thread Vars
    std::vector<int> _tracked_last_req_recv(chameleon_comm_size);
    for(int tmp_i = 0; tmp_i < chameleon_comm_size; tmp_i++)
        _tracked_last_req_recv[tmp_i] = -1;

    DBP("comm_thread_action (enter)\n");

    while(true) {
        // request_manager_cancel.progressRequests();
        #ifdef TRACE
        VT_BEGIN_CONSTRAINED(event_progress_send);
        #endif
        request_manager_send.progressRequests();
        #ifdef TRACE
        VT_END_W_CONSTRAINED(event_progress_send);
        VT_BEGIN_CONSTRAINED(event_progress_recv);
        #endif
        request_manager_receive.progressRequests();
        #ifdef TRACE
        VT_END_W_CONSTRAINED(event_progress_recv);
        #endif

        #if THREAD_ACTIVATION
        while (_flag_comm_threads_sleeping) {
            if(!flag_set) {
                flag_set = 1;
                DBP("comm_thread_action - thread went to sleep again (inside while) - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped.load());
            }
            // dont do anything if the thread is sleeping
            usleep(CHAM_SLEEP_TIME_MICRO_SECS);
            if(_flag_abort_threads) {
                DBP("comm_thread_action (abort)\n");
                free(buffer_load_values);
                int ret_val = 0;
                pthread_exit(&ret_val);
            }
        }
        if(flag_set) {
            DBP("comm_thread_action - woke up again\n");
            flag_set = 0;
            tag_counter_send_tasks = 0;
            // reset last received ids
            for(int tmp_i = 0; tmp_i < chameleon_comm_size; tmp_i++)
                _tracked_last_req_recv[tmp_i] = -1;
            num_threads_in_tw = _num_threads_involved_in_taskwait.load();
        }
        #endif

        // ==============================
        // ========== SEND / EXCHANGE
        // ==============================

        // avoid overwriting request and keep it up to date
        if(!request_gather_created) {
            action_create_gather_request(&num_threads_in_tw, &(transported_load_values[0]), buffer_load_values, &request_gather_out);
            request_gather_created = 1;
            #if CHAM_STATS_RECORD
            time_gather_posted = omp_get_wtime();
            #endif /* CHAM_STATS_RECORD */
        }

        int request_gather_avail;
        #if CHAM_STATS_RECORD
        double cur_time = omp_get_wtime();
        #endif
        MPI_Test(&request_gather_out, &request_gather_avail, &status_gather_out);
        #if CHAM_STATS_RECORD
        cur_time = omp_get_wtime()-cur_time;
        if(cur_time>CHAM_SLOW_COMMUNICATION_THRESHOLD)
          _num_slow_communication_operations++;
        #endif

        if(request_gather_avail) {
            #if CHAM_STATS_RECORD
            _num_load_exchanges_performed++;

            double cur_diff = omp_get_wtime()-time_gather_posted;
            atomic_add_dbl(_time_between_allgather_and_exchange_sum, cur_diff);
            _time_between_allgather_and_exchange_count++;

            // calculate time between two load exchanges discarding sleep times
            if(_comm_thread_load_exchange_happend) {
                cur_diff = omp_get_wtime()-time_last_load_exchange;
                atomic_add_dbl(_time_between_load_exchange_sum, cur_diff);
                _time_between_load_exchange_count++;
            }
            #endif /* CHAM_STATS_RECORD */

            action_handle_gather_request(&event_exchange_outstanding, buffer_load_values, &request_gather_created, &last_known_sum_outstanding, &offload_triggered);

            // set flag that exchange has happend
            if(!_comm_thread_load_exchange_happend) {
                _comm_thread_load_exchange_happend = 1;
            }

            // Handle exit condition here to avoid that iallgather is posted after iteration finished
            bool exit_true = exit_condition_met(0,1);
            if(exit_true){
                _flag_comm_threads_sleeping     = 1;
                _comm_thread_service_stopped    = 1;
                flag_set                        = 1;
                DBP("comm_thread_action - thread went to sleep again due to exit condition\n");
                continue;
            }
            // post Iallgather asap!
           // else if(!request_gather_created) {
           //     action_create_gather_request(&num_threads_in_tw, &(transported_load_values[0]), buffer_load_values, &request_gather_out);
           //     request_gather_created = 1;
           //     #if CHAM_STATS_RECORD
           //     time_gather_posted = omp_get_wtime();
           //     #endif /* CHAM_STATS_RECORD */
           // }

            #if CHAM_STATS_RECORD
            // save last time load exchange happend for current sync cycle
            time_last_load_exchange = omp_get_wtime();
            #endif /* CHAM_STATS_RECORD */
        }

        #if OFFLOAD_ENABLED
        action_task_migration(&event_offload_decision, &offload_triggered, &num_threads_in_tw, tasksToOffload);
        #endif /* OFFLOAD_ENABLED */

        // transfer back data of stolen tasks
        cham_migratable_task_t* cur_task = _stolen_remote_tasks_send_back.pop_front();
        if(cur_task) {
            action_send_back_stolen_tasks(&event_send_back, cur_task, &request_manager_send);
        }

        // ==============================
        // ========== RECV
        // ==============================

        MPI_Status cur_status_receive;
        int flag_open_request_receive = 0;
 
        MPI_Status cur_status_cancel;
        int flag_open_request_cancel = 0;

        MPI_Status cur_status_receiveBack;
        int flag_open_request_receiveBack = 0;

        #if CHAM_STATS_RECORD
        cur_time = omp_get_wtime();
        #endif
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm, &flag_open_request_receive, &cur_status_receive);
        #if CHAM_STATS_RECORD
        cur_time = omp_get_wtime()-cur_time;
        if(cur_time>CHAM_SLOW_COMMUNICATION_THRESHOLD)
          _num_slow_communication_operations++;
        #endif

        #if CHAM_STATS_RECORD
        cur_time = omp_get_wtime();
        #endif
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm_mapped, &flag_open_request_receiveBack, &cur_status_receiveBack);
        #if CHAM_STATS_RECORD
        cur_time = omp_get_wtime()-cur_time;
        if(cur_time>CHAM_SLOW_COMMUNICATION_THRESHOLD)
          _num_slow_communication_operations++;
        #endif
        // MPI_Iprobe(MPI_ANY_SOURCE, 0, chameleon_comm_cancel, &flag_open_request_cancel, &cur_status_cancel);

        // if( flag_open_request_cancel ) {
        //     action_handle_cancel_request(&cur_status_cancel);
        // }

        if ( flag_open_request_receive ) {

            // check last recv id for source rank
            int last_id_handled = _tracked_last_req_recv[cur_status_receive.MPI_SOURCE];
            bool handle_req = last_id_handled != cur_status_receive.MPI_TAG;

            // only handle request if not already done for current phase
            if (handle_req) {
                // track last id recevied for source rank
                _tracked_last_req_recv[cur_status_receive.MPI_SOURCE] = cur_status_receive.MPI_TAG;
                // create requests to receive task
                action_handle_recv_request(&event_receive_tasks, &cur_status_receive, &request_manager_receive);
            }
        }

        if( flag_open_request_receiveBack ) {
            action_handle_recvback_request(&cur_status_receiveBack, &request_manager_receive, &event_recv_back);
        }
    }
}
#pragma endregion CommThread

#ifdef __cplusplus
}
#endif
