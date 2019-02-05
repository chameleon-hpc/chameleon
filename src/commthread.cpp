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
#include <omp.h>
#include <algorithm>
#include <unordered_map>
#include <functional>

#ifdef TRACE
#include "VT.h"
#endif

#define CHAM_SPEEL_TIME_MICRO_SECS 20

#pragma region Variables
// communicator for remote task requests
MPI_Comm chameleon_comm;
// communicator for sending back mapped values
MPI_Comm chameleon_comm_mapped;
// communicator for load information
MPI_Comm chameleon_comm_load;

int chameleon_comm_rank = -1;
int chameleon_comm_size = -1;

//request manager for MPI requests
RequestManager request_manager_receive;
RequestManager request_manager_send;
// array that holds image base addresses
std::vector<intptr_t> _image_base_addresses;

// list with data that has been mapped in map clauses
std::mutex _mtx_data_entry;
std::list<OffloadingDataEntryTy*> _data_entries;

// list with local task entries
// these can either be executed here or offloaded to a different rank
thread_safe_task_list _local_tasks;
std::atomic<int32_t> _num_local_tasks_outstanding(0);

// list with stolen task entries that should be executed
thread_safe_task_list _stolen_remote_tasks;
std::atomic<int32_t> _num_stolen_tasks_outstanding(0);

// list with stolen task entries that need output data transfer
thread_safe_task_list _stolen_remote_tasks_send_back;

#if !OFFLOAD_CREATE_SEPARATE_THREAD
// map that maps tag id's back to local tasks that have been offloaded
std::mutex _mtx_map_tag_to_task;
std::unordered_map<int, TargetTaskEntryTy*> _map_tag_to_task;
#endif

// ====== Info about outstanding jobs (local & stolen) ======
// extern std::mutex _mtx_outstanding_jobs;
std::vector<int32_t> _outstanding_jobs_ranks;
std::atomic<int32_t> _outstanding_jobs_local(0);
std::atomic<int32_t> _outstanding_jobs_sum(0);

// ====== Info about real load that is open or is beeing processed ======
// extern std::mutex _mtx_load_info;
std::vector<int32_t> _load_info_ranks;
int32_t _load_info_local;
int32_t _load_info_sum;
// for now use a single mutex for box info
std::mutex _mtx_load_exchange;

// counter for current number of offloaded tasks
std::atomic<int> _num_offloaded_tasks_outstanding (0);

#if OFFLOAD_BLOCKING
// only enable offloading when a task has finished on local rank (change has been made)
std::atomic<int32_t> _offload_blocked(0);
#endif

// === Constants
const int32_t MAX_BUFFER_SIZE_OFFLOAD_ENTRY = 20480; // 20 KB for testing

// ============== Thread Section ===========
std::mutex _mtx_comm_threads_started;
int _comm_threads_started               = 0;
int _comm_thread_load_exchange_happend  = 0;
int _comm_thread_service_stopped        = 0;

std::mutex _mtx_comm_threads_ended;
int _comm_threads_ended_count           = 0;

// flag that signalizes comm threads to abort their work
std::atomic<int> _flag_abort_threads(0);

// variables to indicate when it is save to break out of taskwait
std::mutex _mtx_taskwait;
std::atomic<int> _flag_comm_threads_sleeping(1);

int _num_threads_involved_in_taskwait       = INT_MAX;
// int _num_threads_entered_taskwait           = 0; // maybe replace with atomic
std::atomic<int32_t> _num_threads_entered_taskwait(0);
std::atomic<int32_t> _num_threads_idle(0);
int _num_ranks_not_completely_idle          = INT_MAX;

pthread_t           _th_receive_remote_tasks;
int                 _th_receive_remote_tasks_created = 0;
pthread_cond_t      _th_receive_remote_tasks_cond    = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     _th_receive_remote_tasks_mutex   = PTHREAD_MUTEX_INITIALIZER;

pthread_t           _th_service_actions;
int                 _th_service_actions_created = 0;
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
void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size);
TargetTaskEntryTy* decode_send_buffer(void * buffer, int mpi_tag);

short pin_thread_to_last_core();
void* offload_action(void *task);

static void send_handler(void* buffer, int tag, int rank);
static void receive_handler(void* buffer, int tag, int rank);
static void send_back_handler(void* buffer, int tag, int rank);
#if !OFFLOAD_CREATE_SEPARATE_THREAD
static void receive_back_handler(void* buffer, int tag, int rank);
#endif
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
    #if CHAM_STATS_RECORD
        cham_stats_init_stats();
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
    err = pthread_create(&_th_receive_remote_tasks, &attr, receive_remote_tasks, NULL);
    if(err != 0)
        handle_error_en(err, "pthread_create - _th_receive_remote_tasks");
    err = pthread_create(&_th_service_actions, &attr, service_thread_action, NULL);
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

int32_t wake_up_comm_threads() {
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

    DBP("wake_up_comm_threads (enter) - _flag_comm_threads_sleeping = %d\n", _flag_comm_threads_sleeping.load());

    #if CHAM_STATS_RECORD
        cham_stats_init_stats();
    #endif
    // determine or set values once
    _num_threads_involved_in_taskwait   = omp_get_num_threads();
    _num_threads_entered_taskwait       = 0;
    _num_threads_idle                   = 0;
    DBP("wake_up_comm_threads       - _num_threads_idle =============> reset: %d\n", _num_threads_idle.load());

    // indicating that this has not happend yet for the current sync cycle
    _comm_thread_load_exchange_happend  = 0;
    _comm_thread_service_stopped        = 0;
    _flag_comm_threads_sleeping         = 0;

    _mtx_taskwait.unlock();
    DBP("wake_up_comm_threads (exit) - _flag_comm_threads_sleeping = %d\n", _flag_comm_threads_sleeping.load());
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
    // first kill all threads
    // err = pthread_cancel(_th_receive_remote_tasks);
    // err = pthread_kill(_th_receive_remote_tasks, SIGKILL);
    
    // safer that way because it leads to severe problems if threads are canceled inside a MPI communication
    _flag_abort_threads = 1;
    // then wait for all threads to finish
    err = pthread_join(_th_receive_remote_tasks, NULL);
    if(err != 0)    handle_error_en(err, "stop_communication_threads - _th_receive_remote_tasks");
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

    _th_receive_remote_tasks_created        = 0;
    _th_service_actions_created             = 0;
    _num_threads_involved_in_taskwait       = INT_MAX;
    _num_threads_entered_taskwait           = 0;
    _num_threads_idle                       = 0;
    #if CHAM_STATS_RECORD && CHAM_STATS_PRINT
    cham_stats_print_stats();
    #endif
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

    #if CHAM_STATS_RECORD && CHAM_STATS_PRINT
        cham_stats_print_stats();
    #endif

    _flag_comm_threads_sleeping             = 1;
    // wait until thread sleeps
    while(!_comm_thread_service_stopped) {
        usleep(CHAM_SPEEL_TIME_MICRO_SECS);
    }
    // DBP("put_comm_threads_to_sleep - service thread stopped = %d\n", _comm_thread_service_stopped);
    _comm_threads_ended_count               = 0;
    _comm_thread_load_exchange_happend      = 0;
    _num_threads_involved_in_taskwait       = INT_MAX;
    _num_threads_entered_taskwait           = 0;
    _num_threads_idle                       = 0;
    DBP("put_comm_threads_to_sleep  - _num_threads_idle =============> reset: %d\n", _num_threads_idle.load());
    _num_ranks_not_completely_idle          = INT_MAX;
    DBP("put_comm_threads_to_sleep - new _num_ranks_not_completely_idle: INT_MAX\n");

    DBP("put_comm_threads_to_sleep (exit)\n");
    _mtx_comm_threads_ended.unlock();
    return CHAM_SUCCESS;
}

short pin_thread_to_last_core() {
    int err;
    int s, j;
    pthread_t thread;
    cpu_set_t current_cpuset;
    cpu_set_t new_cpu_set;
    cpu_set_t final_cpu_set;

    // get current thread to set affinity for    
    thread = pthread_self();
    // get cpuset of complete process
    err = sched_getaffinity(getpid(), sizeof(cpu_set_t), &current_cpuset);
    if(err != 0)
        handle_error_en(err, "sched_getaffinity");
    // also get the number of processing units (here)
    hwloc_topology_t topology;
    hwloc_topology_init(&topology);
    hwloc_topology_load(topology);
    int depth = hwloc_get_type_depth(topology, HWLOC_OBJ_CORE);
    if(depth == HWLOC_TYPE_DEPTH_UNKNOWN) {
        handle_error_en(1001, "hwloc_get_type_depth");
    }
    const long n_physical_cores = hwloc_get_nbobjs_by_depth(topology, depth);
    const long n_logical_cores = sysconf( _SC_NPROCESSORS_ONLN );
    
    // get last hw thread of current cpuset
    long max_core_set = -1;
    for (long i = n_logical_cores; i >= 0; i--) {
        if (CPU_ISSET(i, &current_cpuset)) {
            // DBP("Last core/hw thread in cpuset is %ld\n", i);
            max_core_set = i;
            break;
        }
    }

    // set affinity mask to last core or all hw threads on specific core 
    CPU_ZERO(&new_cpu_set);
    if(max_core_set < n_physical_cores) {
        // Case: there are no hyper threads
        // DBP("Setting thread affinity to core %ld\n", max_core_set);
        CPU_SET(max_core_set, &new_cpu_set);
    } else {
        // Case: there are at least 2 HT per core
        std::string cores(std::to_string(max_core_set));
        CPU_SET(max_core_set, &new_cpu_set);
        for(long i = max_core_set-n_physical_cores; i >= 0; i-=n_physical_cores) {
            cores = std::to_string(i)  + "," + cores;
            CPU_SET(i, &new_cpu_set);
        }
        // DBP("Setting thread affinity to cores %s\n", cores.c_str());
    }
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
    // DBP("Verifying thread affinity: pinned to cores %s\n", final_cores.c_str());
    // // ===== DEBUG

    return CHAM_SUCCESS;
}
#pragma endregion Start/Stop/Pin Communication Threads

#pragma region Handler
static void handler_noop(void* buffer, int tag, int source) {

};

static void send_handler(void* buffer, int tag, int source) {
    free(buffer);
};

static void receive_handler(void* buffer, int tag, int source) {
    TargetTaskEntryTy *task = NULL;

#if CHAM_STATS_RECORD
    double cur_time_decode, cur_time;
    cur_time_decode = omp_get_wtime();
#endif
    task = decode_send_buffer(buffer, tag);
#if CHAM_STATS_RECORD
    cur_time_decode = omp_get_wtime()-cur_time_decode;
    atomic_add_dbl(_time_decode_sum, cur_time_decode);
    _time_decode_count++;
#endif
#if OFFLOAD_DATA_PACKING_TYPE == 1
    MPI_Request *requests = new MPI_Request[task->arg_num];
    DBP("offload_action - receiving data from rank %d with tag: %d\n", source, tag);


#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime();
#endif
    for(int32_t i=0; i<task->arg_num; i++) {
        int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        if(is_lit) {
            MPI_Irecv(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, source, tag, chameleon_comm, &requests[i]);
        } else {
	        MPI_Irecv(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, source, tag, chameleon_comm, &requests[i]);
        }
        print_arg_info("receive_handler - receiving argument", task, i);
    }
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_comm_recv_task_sum, cur_time);    
#endif
    request_manager_receive.submitRequests( tag, 
                                    source,
                                    task->arg_num, 
                                    requests,
                                    true,        //TODO: we need to block before the task can be submitted!      
                                    handler_noop,
                                    recvData);
    delete[] requests;
#endif

    // set information for sending back results/updates if necessary
    task->source_mpi_rank   = source;
    task->source_mpi_tag    = tag;

    // add task to stolen list and increment counter
    _stolen_remote_tasks.push_back(task);

    _mtx_load_exchange.lock();
    _num_stolen_tasks_outstanding++;
    _load_info_local++;
    trigger_update_outstanding();
    _mtx_load_exchange.unlock();

    free(buffer);
}

static void send_back_handler(void* buffer, int tag, int source) {
    free(buffer);
}

#if !OFFLOAD_CREATE_SEPARATE_THREAD
static void receive_back_handler(void* buffer, int tag, int source) {
    DBP("receive_remote_tasks - receiving output data from rank %d for tag: %d\n", source, 
                                                                                   tag); 

    bool match = false;
    _mtx_map_tag_to_task.lock();
    std::unordered_map<int ,TargetTaskEntryTy*>::const_iterator got = _map_tag_to_task.find(tag);
    match = got != _map_tag_to_task.end();
    if(match) {
        _map_tag_to_task.erase(tag);
    }
    _mtx_map_tag_to_task.unlock(); 

    TargetTaskEntryTy *task_entry;
    if(match) {
        task_entry = got->second;
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
       _mtx_unfinished_locally_created_tasks.lock();
       _unfinished_locally_created_tasks.remove(task_entry->task_id);
       _mtx_unfinished_locally_created_tasks.unlock();

       // decrement counter if offloading + receiving results finished
       _mtx_load_exchange.lock();
       _num_local_tasks_outstanding--;
       trigger_update_outstanding();
       _mtx_load_exchange.unlock();
    }
}
#endif
#pragma endregion

#pragma region Offloading / Packing
int32_t offload_task_to_rank(OffloadEntryTy *entry) {
#ifdef TRACE
    static int event_offload = -1;
    std::string event_offload_name = "offload_task";
    if(event_offload == -1) 
        int ierr = VT_funcdef(event_offload_name.c_str(), VT_NOCLASS, &event_offload);
    VT_begin(event_offload);
#endif 
    int has_outputs = entry->task_entry->HasAtLeastOneOutput();
    DBP("offload_task_to_rank (enter) - task_entry (task_id=%d) " DPxMOD ", num_args: %d, rank: %d, has_output: %d\n", entry->task_entry->task_id, DPxPTR(entry->task_entry->tgt_entry_ptr), entry->task_entry->arg_num, entry->target_rank, has_outputs);

    _mtx_load_exchange.lock();
    _load_info_local--;
    trigger_update_outstanding();
    _mtx_load_exchange.unlock();

#if OFFLOAD_CREATE_SEPARATE_THREAD
    // explicitly make thread joinable to be portable
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    // Only create additional thread if there are output variables
    if(has_outputs) {
       pthread_t *tmp_new_thread = (pthread_t*)malloc(sizeof(pthread_t));
       int err;
       err = pthread_create(tmp_new_thread, &attr, offload_action, (void*)entry);
       if(err != 0)
           handle_error_en(err, "offload_task_to_rank - pthread_create");
    } else {
        // no addition thread needed
        offload_action((void*)entry);
        _num_offloaded_tasks_outstanding++;
    }
#else
    // directly use base function
    offload_action((void*)entry);
    _num_offloaded_tasks_outstanding++;
#endif

#if CHAM_STATS_RECORD
    _num_tasks_offloaded++;
#endif

    DBP("offload_task_to_rank (exit)\n");
#ifdef TRACE
    VT_end(event_offload);
#endif
    return CHAM_SUCCESS;
}

void* offload_action(void *v_entry) {
#if OFFLOAD_CREATE_SEPARATE_THREAD
    pin_thread_to_last_core();
    DBP("offload_action (create) - created new thread and pinned to last core\n");
#endif
    // parse argument again, necessary since it can be used for thread and pure as well
    OffloadEntryTy *entry = (OffloadEntryTy *) v_entry;
#ifdef TRACE
    static int event_offload_send = -1;
    std::string event_offload_send_name = "offload_send";
    if(event_offload_send == -1) 
        int ierr = VT_funcdef(event_offload_send_name.c_str(), VT_NOCLASS, &event_offload_send);

     VT_begin(event_offload_send);
#endif
    int has_outputs = entry->task_entry->HasAtLeastOneOutput();
    DBP("offload_action (enter) - task_entry (task_id=%d) " DPxMOD ", num_args: %d, rank: %d, has_output: %d\n", entry->task_entry->task_id, DPxPTR(entry->task_entry->tgt_entry_ptr), entry->task_entry->arg_num, entry->target_rank, has_outputs);
    
    // use unique task entry as a tag
    int tmp_tag = entry->task_entry->task_id;

    // encode buffer
    int32_t buffer_size = 0;
    void *buffer = NULL;

#if CHAM_STATS_RECORD
    double cur_time;
    cur_time = omp_get_wtime();
#endif
    buffer = encode_send_buffer(entry->task_entry, &buffer_size);
#if OFFLOAD_DATA_PACKING_TYPE == 0
    // RELP("Packing Type: Buffer\n");
    int n_requests = 1;
#elif OFFLOAD_DATA_PACKING_TYPE == 1
    // RELP("Packing Type: Zero Copy\n");
    int n_requests = 1 + entry->task_entry->arg_num;
#endif
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_encode_sum, cur_time);
    _time_encode_count++;
#endif

#ifdef TRACE
    VT_begin(event_offload_send);
#endif    
    // send data to target rank
    DBP("offload_action - sending data to target rank %d with tag: %d\n", entry->target_rank, tmp_tag);

#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime();
#endif
    MPI_Request *requests = new MPI_Request[n_requests];
    MPI_Isend(buffer, buffer_size, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm, &requests[0]);

#if OFFLOAD_DATA_PACKING_TYPE == 1
    for(int i=0; i<entry->task_entry->arg_num; i++) {
        int is_lit      = entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        if(is_lit) {
            MPI_Isend(&entry->task_entry->arg_hst_pointers[i], entry->task_entry->arg_sizes[i], MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm, &requests[i+1]);
        }
        else{
            MPI_Isend(entry->task_entry->arg_hst_pointers[i], entry->task_entry->arg_sizes[i], MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm, &requests[i+1]);
        } 
        print_arg_info("offload_action - sending argument", entry->task_entry, i);
   }
#endif
#if !OFFLOAD_CREATE_SEPARATE_THREAD
    if(has_outputs) {
        _mtx_map_tag_to_task.lock();
        _map_tag_to_task.insert(std::make_pair(tmp_tag, entry->task_entry));
        _mtx_map_tag_to_task.unlock();
    }
#endif
#if CHAM_STATS_RECORD                            
    cur_time = omp_get_wtime()-cur_time;
    atomic_add_dbl(_time_comm_send_task_sum, cur_time);
    _time_comm_send_task_count++;
#endif
    request_manager_send.submitRequests(  tmp_tag, entry->target_rank, n_requests, 
                                requests,
                                MPI_BLOCKING,
                                send_handler,
                                send,
                                buffer);
    delete[] requests;
#ifdef TRACE
    VT_end(event_offload_send);
#endif

    if(has_outputs) {
#if OFFLOAD_CREATE_SEPARATE_THREAD
#ifdef TRACE
        static int event_offload_wait_recv = -1;
        std::string event_offload_wait_recv_name = "offload_wait_recv";
        if(event_offload_wait_recv == -1) 
            int ierr = VT_funcdef(event_offload_wait_recv_name.c_str(), VT_NOCLASS, &event_offload_wait_recv);
        VT_begin(event_offload_wait_recv);
#endif
        DBP("offload_action - waiting for output data from rank %d for (task_id=%d)\n", entry->target_rank, tmp_tag);
        MPI_Status cur_status;
        int recv_buff_size;
        int flag = 0;

        while(!flag) {
            MPI_Iprobe(entry->target_rank, tmp_tag, chameleon_comm_mapped, &flag, &cur_status);
            usleep(CHAM_SPEEL_TIME_MICRO_SECS);
        }

        MPI_Get_count(&cur_status, MPI_BYTE, &recv_buff_size);
        // temp buffer to retreive output data from target rank to be able to update host pointers again
        void * temp_buffer = malloc(recv_buff_size);
#if CHAM_STATS_RECORD
        cur_time = omp_get_wtime();
#endif
        MPI_Recv(temp_buffer, recv_buff_size, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm_mapped, MPI_STATUS_IGNORE);
#if CHAM_STATS_RECORD
        cur_time = omp_get_wtime()-cur_time;
        atomic_add_dbl(_time_comm_back_recv_sum, cur_time);
        _time_comm_back_recv_count++;
#endif
        DBP("offload_action - receiving output data from rank %d for (task_id=%d)\n", entry->target_rank, tmp_tag);
        // copy results back to source pointers with memcpy
        char * cur_ptr = (char*) temp_buffer;
        for(int i = 0; i < entry->task_entry->arg_num; i++) {
            int is_lit      = entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_from     = entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;

            if(entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                print_arg_info("offload_action", entry->task_entry, i);
                    
                // we already have information about size and data type
                memcpy(entry->task_entry->arg_hst_pointers[i], cur_ptr, entry->task_entry->arg_sizes[i]);
                cur_ptr += entry->task_entry->arg_sizes[i];
            }
        }
        // free buffer again
        free(temp_buffer);
#ifdef TRACE
        VT_end(event_offload_wait_recv);
#endif
        // mark locally created task finished
        _mtx_unfinished_locally_created_tasks.lock();
        _unfinished_locally_created_tasks.remove(entry->task_entry->task_id);
        _mtx_unfinished_locally_created_tasks.unlock();

        // decrement counter if offloading + receiving results finished
        _mtx_load_exchange.lock();
        _num_local_tasks_outstanding--;
        trigger_update_outstanding();
        _mtx_load_exchange.unlock();
#endif
    } /*else {
        // mark locally created task finished
        _mtx_unfinished_locally_created_tasks.lock();
        _unfinished_locally_created_tasks.remove(entry->task_entry->task_id);
        _mtx_unfinished_locally_created_tasks.unlock();

        // decrement counter if offloading finished
        _mtx_load_exchange.lock();
        _num_local_tasks_outstanding--;
        trigger_update_outstanding();
        _mtx_load_exchange.unlock();
    }*/

    DBP("offload_action (exit)\n");
    return nullptr;
}

void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size) {
#ifdef TRACE
    static int event_encode = -1;
    std::string event_encode_name = "encode";
    if(event_encode == -1) 
        int ierr = VT_funcdef(event_encode_name.c_str(), VT_NOCLASS, &event_encode);
    VT_begin(event_encode);
#endif 
    DBP("encode_send_buffer (enter) - task_entry (task_id=%d) " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", task->task_id, DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

    // FORMAT:
    //      1. target function pointer = address (intptr_t)
    //      2. image index
    //      3. offset of entry point inside image
    //      4. number of arguments = int32_t
    //      5. array with argument types = n_args * int64_t
    //      6. array with argument offsets = n_args * int64_t
    //      7. array with length of argument pointers = n_args * int64_t
    //      8. array with values

    int total_size = sizeof(intptr_t)           // 1. target entry pointer
        + sizeof(int32_t)                       // 2. img index
        + sizeof(ptrdiff_t)                     // 3. offset inside image
        + sizeof(int32_t)                       // 4. number of arguments
        + task->arg_num * sizeof(int64_t)       // 5. argument sizes
        + task->arg_num * sizeof(ptrdiff_t)     // 6. offsets
        + task->arg_num * sizeof(int64_t);      // 7. argument types

#if OFFLOAD_DATA_PACKING_TYPE == 0
    for(int i = 0; i < task->arg_num; i++) {
        total_size += task->arg_sizes[i];
    }
#endif

#if CHAMELEON_TOOL_SUPPORT
    int32_t task_tool_buf_size = 0;
    void *task_tool_buffer = nullptr;

    if(cham_t_status.enabled && cham_t_status.cham_t_callback_encode_task_tool_data && cham_t_status.cham_t_callback_decode_task_tool_data) {
        task_tool_buffer = cham_t_status.cham_t_callback_encode_task_tool_data(task, &(task->task_tool_data), &task_tool_buf_size);
        total_size += sizeof(int32_t) + task_tool_buf_size; // size information + buffer size
    }
#endif

    // allocate memory for transfer
    char *buff = (char *) malloc(total_size);
    char *cur_ptr = (char *)buff;

    // 1. target entry address
    ((intptr_t *) cur_ptr)[0] = (intptr_t) task->tgt_entry_ptr;
    cur_ptr += sizeof(intptr_t);

    // 2. img index
    ((int32_t *) cur_ptr)[0] = task->idx_image;
    cur_ptr += sizeof(int32_t);

    // 3. offset
    ((ptrdiff_t *) cur_ptr)[0] = task->entry_image_offset;
    cur_ptr += sizeof(ptrdiff_t);

    // 4. number of arguments
    ((int32_t *) cur_ptr)[0] = task->arg_num;
    cur_ptr += sizeof(int32_t);

    // 5. argument sizes
    memcpy(cur_ptr, &(task->arg_sizes[0]), task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 6. offsets
    memcpy(cur_ptr, &(task->arg_tgt_offsets[0]), task->arg_num * sizeof(ptrdiff_t));
    cur_ptr += task->arg_num * sizeof(ptrdiff_t);

    // 7. argument types
    memcpy(cur_ptr, &(task->arg_types[0]), task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

#if OFFLOAD_DATA_PACKING_TYPE == 0
    // 8. loop through arguments and copy values
    for(int32_t i = 0; i < task->arg_num; i++) {
        int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        int is_from     = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;

        print_arg_info("encode_send_buffer", task, i);

        // copy value from host pointer directly
        if(is_lit) {
            ((intptr_t *) cur_ptr)[0] = (intptr_t) task->arg_hst_pointers[i];
        } else {
            memcpy(cur_ptr, (char*)task->arg_hst_pointers[i], task->arg_sizes[i]);
        }
        cur_ptr += task->arg_sizes[i];
    }
#endif

#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_encode_task_tool_data && cham_t_status.cham_t_callback_decode_task_tool_data) {
        // remember size of buffer
        ((int32_t *) cur_ptr)[0] = task_tool_buf_size;
        cur_ptr += sizeof(int32_t);

        memcpy(cur_ptr, task_tool_buffer, task_tool_buf_size);
        cur_ptr += task_tool_buf_size;
        // clean up again
        free(task_tool_buffer);
    }
#endif

    // set output size
    *buffer_size = total_size;
#ifdef TRACE
    VT_end(event_encode);
#endif
    return buff;
}

TargetTaskEntryTy* decode_send_buffer(void * buffer, int mpi_tag) {
#ifdef TRACE
    static int event_decode = -1;
    std::string event_decode_name = "decode";
    if(event_decode == -1) 
        int ierr = VT_funcdef(event_decode_name.c_str(), VT_NOCLASS, &event_decode);
    VT_begin(event_decode);
#endif 
    // init new task
    TargetTaskEntryTy* task = new TargetTaskEntryTy();
    // actually we use the global task id as tag
    task->task_id           = mpi_tag;
    task->is_remote_task    = 1;

    // current pointer position
    char *cur_ptr = (char*) buffer;

    // 1. target function pointer
    task->tgt_entry_ptr = ((intptr_t *) cur_ptr)[0];
    cur_ptr += sizeof(intptr_t);

    // 2. img index
    task->idx_image = ((int32_t *) cur_ptr)[0];
    cur_ptr += sizeof(int32_t);

    // 3. offset
    task->entry_image_offset = ((ptrdiff_t *) cur_ptr)[0];
    cur_ptr += sizeof(ptrdiff_t);

    // 4. number of arguments
    task->arg_num = ((int32_t *) cur_ptr)[0];
    cur_ptr += sizeof(int32_t);

    DBP("decode_send_buffer (enter) - task_entry (task_id=%d): " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", task->task_id, DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

    // we need a mapping to process local task entry points
    intptr_t local_img_base     = _image_base_addresses[task->idx_image];
    intptr_t local_entry        = local_img_base + task->entry_image_offset;
    DBP("decode_send_buffer - mapping remote entry point from: " DPxMOD " to local: " DPxMOD "\n", DPxPTR(task->tgt_entry_ptr), DPxPTR(local_entry));
    task->tgt_entry_ptr         = local_entry;

    // resize data structure
    task->ReSizeArrays(task->arg_num);
    
    // 5. argument sizes
    memcpy(&(task->arg_sizes[0]), cur_ptr, task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 6. argument types
    memcpy(&(task->arg_tgt_offsets[0]), cur_ptr, task->arg_num * sizeof(ptrdiff_t));
    cur_ptr += task->arg_num * sizeof(ptrdiff_t);

    // 7. offsets
    memcpy(&(task->arg_types[0]), cur_ptr, task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 8. loop through arguments and copy values
    for(int32_t i = 0; i < task->arg_num; i++) {
        int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        int is_from     = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;

#if OFFLOAD_DATA_PACKING_TYPE == 0
        // copy value from host pointer directly
        if(is_lit) {
            intptr_t tmp_ptr = ((intptr_t *) cur_ptr)[0];
            task->arg_hst_pointers[i] = (void *) tmp_ptr;
        } else {
            // need to allocate new memory
            void * new_mem = malloc(task->arg_sizes[i]);
            memcpy(new_mem, cur_ptr, task->arg_sizes[i]);
            task->arg_hst_pointers[i] = new_mem;
        }
        // increment pointer
        cur_ptr += task->arg_sizes[i];
#elif OFFLOAD_DATA_PACKING_TYPE == 1
        // copy value from host pointer directly
        if(!is_lit) {
            // need to allocate new memory
            void * new_mem = malloc(task->arg_sizes[i]);
            task->arg_hst_pointers[i] = new_mem;
        }
#endif
        print_arg_info("decode_send_buffer", task, i);
    }

#if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_encode_task_tool_data && cham_t_status.cham_t_callback_decode_task_tool_data) {
        // first get size of buffer
        int32_t task_tool_buf_size = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);
        cham_t_status.cham_t_callback_decode_task_tool_data(task, &(task->task_tool_data), (void*)cur_ptr, task_tool_buf_size);
    }
#endif

#ifdef TRACE
    VT_end(event_decode);
#endif 
    return task;
}
#pragma endregion Offloading / Packing

#pragma region Thread Receive
// should run in a single thread that is always waiting for incoming requests
void* receive_remote_tasks(void* arg) {
#ifdef TRACE
    static int event_receive_tasks = -1;
    std::string event_receive_tasks_name = "receive_task";
    if(event_receive_tasks == -1) 
        int ierr = VT_funcdef(event_receive_tasks_name.c_str(), VT_NOCLASS, &event_receive_tasks);

#if !OFFLOAD_CREATE_SEPARATE_THREAD
    static int event_recv_back = -1;
    std::string event_recv_back_name = "receive_back";
    if(event_recv_back == -1) 
        int ierr = VT_funcdef(event_recv_back_name.c_str(), VT_NOCLASS, &event_recv_back);
#endif
#endif 
    pin_thread_to_last_core();
    DBP("receive_remote_tasks (enter)\n");

    int32_t res;
    // intention to reuse buffer over and over again
    //int cur_max_buff_size = MAX_BUFFER_SIZE_OFFLOAD_ENTRY;
    void * buffer;// = malloc(cur_max_buff_size);
    int recv_buff_size = 0;
    int flag_set = 0;
 
    double cur_time;

    while(true) {
        request_manager_receive.progressRequests();

        // first check transmission and make sure that buffer has enough memory
        MPI_Status cur_status_receive;
        int flag_open_request_receive = 0;

#if THREAD_ACTIVATION
        while (_flag_comm_threads_sleeping) {
            if(!flag_set) {
                flag_set                        = 1;
                DBP("receive_remote_tasks - thread went to sleep again (inside while) - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped);
            }
            // dont do anything if the thread is sleeping
            usleep(CHAM_SPEEL_TIME_MICRO_SECS);
            // DBP("receive_remote_tasks - thread sleeping\n");
            if(_flag_abort_threads) {
                DBP("receive_remote_tasks (abort)\n");
                //free(buffer);
                int ret_val = 0;
                pthread_exit(&ret_val);
            }
        }
        if(flag_set) {
            DBP("receive_remote_tasks - woke up again - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped);
            flag_set = 0;
        }
#endif

#if OFFLOAD_CREATE_SEPARATE_THREAD
        while(!flag_open_request_receive) {
#else
            MPI_Status cur_status_receiveBack;
            int flag_open_request_receiveBack = 0;

            while(!flag_open_request_receive && !flag_open_request_receiveBack) {
#endif
                usleep(CHAM_SPEEL_TIME_MICRO_SECS);
                // check whether thread should be aborted
                if(_flag_abort_threads && _num_offloaded_tasks_outstanding==0) {

                    DBP("receive_remote_tasks (abort), outstanding requests: %d\n", request_manager_receive.getNumberOfOutstandingRequests());
                    while(!(request_manager_receive.getNumberOfOutstandingRequests()==0)) {
                        request_manager_receive.progressRequests();
                    }
                    //free(buffer);
                    int ret_val = 0;
                    pthread_exit(&ret_val);
                }
#if THREAD_ACTIVATION
                if(_flag_comm_threads_sleeping) {
                    break;
                }
#endif
                MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm, &flag_open_request_receive, &cur_status_receive);
#if OFFLOAD_CREATE_SEPARATE_THREAD
            }
#else
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm_mapped, &flag_open_request_receiveBack, &cur_status_receiveBack);
        }

#if THREAD_ACTIVATION
        // if threads have been put to sleep start from beginning to end up in sleep mode
        if(_flag_comm_threads_sleeping) {
            DBP("receive_remote_tasks - thread went to sleep again\n");
            continue;
        }
#endif

        if( flag_open_request_receiveBack ) {
            bool match = false;
	    _mtx_map_tag_to_task.lock();
            std::unordered_map<int ,TargetTaskEntryTy*>::const_iterator got = _map_tag_to_task.find(cur_status_receiveBack.MPI_TAG);
            match = got != _map_tag_to_task.end();
            _mtx_map_tag_to_task.unlock();

            if(match) {
#if OFFLOAD_DATA_PACKING_TYPE == 0
            //entry is removed in receiveBackHandler!
            // receive data back
	        MPI_Get_count(&cur_status_receiveBack, MPI_BYTE, &recv_buff_size);
                buffer = malloc(recv_buff_size);
#ifdef TRACE
                VT_begin(event_recv_back);
#endif
 
#if CHAM_STATS_RECORD
                cur_time = omp_get_wtime();
#endif
                MPI_Request request;
                MPI_Irecv(buffer, recv_buff_size, MPI_BYTE, cur_status_receiveBack.MPI_SOURCE, cur_status_receiveBack.MPI_TAG,
                                                                                      chameleon_comm_mapped, &request);
#if CHAM_STATS_RECORD
                cur_time = omp_get_wtime()-cur_time;
                atomic_add_dbl(_time_comm_back_recv_sum, cur_time);
                _time_comm_back_recv_count++;
#endif
                request_manager_receive.submitRequests( cur_status_receiveBack.MPI_TAG, cur_status_receiveBack.MPI_SOURCE, 1, 
                                                        &request,
                                                        MPI_BLOCKING,
                                                        receive_back_handler,
                                                        recvBack,
                                                        buffer);
      
#ifdef TRACE
                VT_end(event_recv_back);
#endif

#elif OFFLOAD_DATA_PACKING_TYPE == 1

#ifdef TRACE
                VT_begin(event_recv_back);
#endif
  
                //get task and receive data directly into task data
	        TargetTaskEntryTy *task_entry;
                task_entry = got->second;

#if CHAM_STATS_RECORD
                cur_time = omp_get_wtime();
#endif   
                MPI_Request *requests = new MPI_Request[task_entry->arg_num];  
                int j = 0;
                for(int i = 0; i < task_entry->arg_num; i++) {
                    if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                        MPI_Irecv(task_entry->arg_hst_pointers[i], task_entry->arg_sizes[i], MPI_BYTE, cur_status_receiveBack.MPI_SOURCE, cur_status_receiveBack.MPI_TAG,
                                                                                     chameleon_comm_mapped, &requests[j++]);
                    }
                }
#if CHAM_STATS_RECORD
                cur_time = omp_get_wtime()-cur_time;
                atomic_add_dbl(_time_comm_back_recv_sum, cur_time);
                _time_comm_back_recv_count++;
#endif      
		request_manager_receive.submitRequests( cur_status_receiveBack.MPI_TAG, cur_status_receiveBack.MPI_SOURCE, j, 
                                                        &requests[0],
                                                        MPI_BLOCKING,
                                                        receive_back_handler,
                                                        recvBack,
                                                        nullptr);
                delete[] requests;
#ifdef TRACE
                VT_end(event_recv_back);
#endif
            
#endif
            }
	}
#endif
        // receive new task
        if ( flag_open_request_receive ) {
#ifdef TRACE
            VT_begin(event_receive_tasks);
#endif
            MPI_Get_count(&cur_status_receive, MPI_BYTE, &recv_buff_size);
            buffer = malloc(recv_buff_size);
     
            MPI_Request request = MPI_REQUEST_NULL;
            // now receive at least meta data
#if CHAM_STATS_RECORD
            cur_time = omp_get_wtime();
#endif
            res = MPI_Irecv(buffer, recv_buff_size, MPI_BYTE, cur_status_receive.MPI_SOURCE, cur_status_receive.MPI_TAG, chameleon_comm, &request);
#if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            atomic_add_dbl(_time_comm_recv_task_sum, cur_time);
            _time_comm_recv_task_count++;
#endif
            request_manager_receive.submitRequests( cur_status_receive.MPI_TAG, 
                                            cur_status_receive.MPI_SOURCE,
                                            1, 
                                            &request,
                                            MPI_BLOCKING,
                                            receive_handler,
                                            recv,
                                            buffer);
#ifdef TRACE
            VT_end(event_receive_tasks);
#endif
        }
    }
}
#pragma endregion Thread Receive

#pragma region Thread Send / Service
void* service_thread_action(void *arg) {
#ifdef TRACE
    static int event_exchange_outstanding = -1;
    std::string exchange_outstanding_name = "exchange_outstanding";
    if(event_exchange_outstanding == -1)
        int ierr = VT_funcdef(exchange_outstanding_name.c_str(), VT_NOCLASS, &event_exchange_outstanding);

    static int event_offload_decision = -1;
    std::string event_offload_decision_name = "offload_decision";
    if(event_offload_decision == -1)
        int ierr = VT_funcdef(event_offload_decision_name.c_str(), VT_NOCLASS, &event_offload_decision);
    
    static int event_send_back = -1;
    std::string event_send_back_name = "send_back";
    if(event_send_back == -1)
        int ierr = VT_funcdef(event_send_back_name.c_str(), VT_NOCLASS, &event_send_back);
#endif

    pin_thread_to_last_core();
    
    // trigger signal to tell that thread is running now
    pthread_mutex_lock( &_th_service_actions_mutex );
    _th_service_actions_created = 1; 
    pthread_cond_signal( &_th_service_actions_cond );
    pthread_mutex_unlock( &_th_service_actions_mutex );

    int err;
    MPI_Request request_out;
    MPI_Status status_out;
    int request_created = 0;
    int offload_triggered = 0;
    int last_known_sum_outstanding = -1;
    int flag_set = 0;

    int transported_load_values[3];
    int * buffer_load_values = (int*) malloc(sizeof(int)*3*chameleon_comm_size);
    std::vector<int32_t> tasksToOffload(chameleon_comm_size);

    DBP("service_thread_action (enter)\n");
    while(true) {

        request_manager_send.progressRequests();

        TargetTaskEntryTy* cur_task = nullptr;

        #if THREAD_ACTIVATION
        while (_flag_comm_threads_sleeping) {
            if(!flag_set) {
                _comm_thread_service_stopped    = 1;
                flag_set                        = 1;
                DBP("service_thread_action - thread went to sleep again (inside while) - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped);
            }
            // dont do anything if the thread is sleeping
            usleep(CHAM_SPEEL_TIME_MICRO_SECS);
            // DBP("service_thread_action - thread sleeping\n");
            if(_flag_abort_threads) {
                DBP("service_thread_action (abort)\n");
                free(buffer_load_values);
                int ret_val = 0;
                pthread_exit(&ret_val);
            }
        }
        if(flag_set) {
            DBP("service_thread_action - woke up again - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped);
            flag_set = 0;
        }
        #endif

        // ================= Load / Outstanding Jobs Section =================
        // exchange work load here before reaching the abort section
        // this is a collective call and needs to be performed at least once

        // avoid overwriting request or keep it up to date?
        // not 100% sure if overwriting a request is a bad idea or makes any problems in MPI
        if(!request_created) {
#ifdef TRACE
            VT_begin(event_exchange_outstanding);
#endif
            _mtx_load_exchange.lock();
            int32_t local_load_representation;
            int32_t num_ids_local;
            int64_t* ids_local = _local_tasks.get_task_ids(&num_ids_local);
            int32_t num_ids_stolen;
            int64_t* ids_stolen = _stolen_remote_tasks.get_task_ids(&num_ids_stolen);
#if CHAMELEON_TOOL_SUPPORT
            if(cham_t_status.enabled && cham_t_status.cham_t_callback_determine_local_load) {
                local_load_representation = cham_t_status.cham_t_callback_determine_local_load(ids_local, num_ids_local, ids_stolen, num_ids_stolen);
            } else {
                local_load_representation = getDefaultLoadInformationForRank(ids_local, num_ids_local, ids_stolen, num_ids_stolen);
            }
#else 
            local_load_representation = getDefaultLoadInformationForRank(ids_local, num_ids_local, ids_stolen, num_ids_stolen);
#endif
            // clean up again
            free(ids_local);
            free(ids_stolen);

            int tmp_val = _num_threads_idle.load() < _num_threads_involved_in_taskwait ? 1 : 0;
            // DBP("service_thread_action - my current value for rank_not_completely_in_taskwait: %d\n", tmp_val);
            transported_load_values[0] = tmp_val;
            transported_load_values[1] = _outstanding_jobs_local.load();
            transported_load_values[2] = local_load_representation;
            _mtx_load_exchange.unlock();

            if(_flag_comm_threads_sleeping) {
                _comm_thread_service_stopped    = 1;
                flag_set                        = 1;
                DBP("service_thread_action - thread went to sleep again before Iallgather - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped);
                continue;
            }

            MPI_Iallgather(&transported_load_values[0], 3, MPI_INT, buffer_load_values, 3, MPI_INT, chameleon_comm_load, &request_out);
            request_created = 1;
#ifdef TRACE
            VT_end(event_exchange_outstanding);
#endif
        }

        int flag_request_avail;
        MPI_Test(&request_out, &flag_request_avail, &status_out);
        if(flag_request_avail) {
#ifdef TRACE
            VT_begin(event_exchange_outstanding);
#endif
            // sum up that stuff
            // DBP("service_thread_action - gathered new load info\n");
            int32_t old_val_n_ranks                 = _num_ranks_not_completely_idle;
            int32_t old_outstanding_sum             = _outstanding_jobs_sum;
            int32_t sum_ranks_not_completely_idle   = 0;
            int32_t sum_outstanding                 = 0;
            int32_t sum_load                        = 0;

            for(int j = 0; j < chameleon_comm_size; j++) {
                // DBP("service_thread_action - values for rank %d: all_in_tw=%d, outstanding_jobs=%d, load=%d\n", j, buffer_load_values[j*2], buffer_load_values[(j*2)+1], buffer_load_values[(j*2)+2]);
                int tmp_tw                              = buffer_load_values[j*3];
                _outstanding_jobs_ranks[j]              = buffer_load_values[(j*3)+1];
                _load_info_ranks[j]                     = buffer_load_values[(j*3)+2];
                sum_outstanding                         += _outstanding_jobs_ranks[j];
                sum_load                                += _load_info_ranks[j];
                sum_ranks_not_completely_idle           += tmp_tw;
                // DBP("load info from rank %d = %d\n", j, _load_info_ranks[j]);
            }

            _num_ranks_not_completely_idle      = sum_ranks_not_completely_idle;
            // if(old_val_n_ranks != sum_ranks_not_completely_idle || old_outstanding_sum != sum_outstanding) {
            //     DBP("service_thread_action - _num_ranks_not_completely_idle: old=%d new=%d\n", old_val_n_ranks, sum_ranks_not_completely_idle);
            //     DBP("service_thread_action - _outstanding_jobs_sum: old=%d new=%d\n", old_outstanding_sum, sum_outstanding);
            // }
            _outstanding_jobs_sum               = sum_outstanding;
            _load_info_sum                      = sum_load;
            // DBP("complete summed load = %d\n", _load_info_sum);
            // set flag that exchange has happend
            if(!_comm_thread_load_exchange_happend)
                _comm_thread_load_exchange_happend = 1;
            // reset flag
            request_created = 0;
#if OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED
            if(last_known_sum_outstanding == -1) {
                last_known_sum_outstanding = sum_outstanding;
                offload_triggered = 0;
            } else {
                // check whether changed.. only allow new offload after change
                if(last_known_sum_outstanding != sum_outstanding) {
                    last_known_sum_outstanding = sum_outstanding;
                    offload_triggered = 0;
                }
            }
#else // OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED
            offload_triggered = 0;
#endif

#ifdef TRACE
            VT_end(event_exchange_outstanding);
#endif
            // Handle exit condition here to avoid that iallgather is posted after iteration finished
            if(exit_condition_met(1)){
                _flag_comm_threads_sleeping     = 1;
                _comm_thread_service_stopped    = 1;
                flag_set                        = 1;
                DBP("service_thread_action - thread went to sleep again due to exit condition - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped);
                continue;
            }
        }

        // check whether to abort thread
        if(_flag_abort_threads) {
            DBP("service_thread_action (abort), outstanding requests: %d\n", request_manager_send.getNumberOfOutstandingRequests());
            while(!(request_manager_send.getNumberOfOutstandingRequests()==0)) {
              request_manager_send.progressRequests();
            }
            free(buffer_load_values);
            _comm_thread_service_stopped = 1;
            int ret_val = 0;
            pthread_exit(&ret_val);
        }

        #if THREAD_ACTIVATION
        // if threads have been put to sleep start from beginning to end up in sleep mode
        if(_flag_comm_threads_sleeping) {
            _comm_thread_service_stopped    = 1;
            flag_set                        = 1;
            DBP("service_thread_action - thread went to sleep again - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped);
            continue;
        }
        #endif

#if !FORCE_OFFLOAD_MASTER_WORKER && OFFLOAD_ENABLED
        // ================= Offloading Section =================
        // only check for offloading if enough local tasks available and exchange has happend at least once
        if(_comm_thread_load_exchange_happend && _local_tasks.size() > 1 && !offload_triggered) {

#if OFFLOAD_BLOCKING
            if(!_offload_blocked) {
#endif
#ifdef TRACE
            VT_begin(event_offload_decision);
#endif
            int cur_load = _load_info_ranks[chameleon_comm_rank];
            
            // Strategies for speculative load exchange
            // - If we find a rank with load = 0 ==> offload directly
            // - Should we look for the minimum? Might be a critical part of the program because several ranks might offload to that rank
            // - Just offload if there is a certain difference between min and max load to avoid unnecessary offloads
            // - Sorted approach: rank with highest load should offload to rank with minimal load
            // - Be careful about balance between computational complexity of calculating the offload target and performance gain that can be achieved
            
//            // check other ranks whether there is a rank with low load; start at current position
//            for(int k = 1; k < chameleon_comm_size; k++) {
//                 int tmp_idx = (chameleon_comm_rank + k) % chameleon_comm_size;
//                 if(_load_info_ranks[tmp_idx] == 0) {
//                     // Direct offload if thread found that has nothing to do
//                     TargetTaskEntryTy *cur_task = _local_tasks.pop_front();
//                     if(cur_task == nullptr)
//                         break;
// #ifdef TRACE
//             VT_end(event_offload_decision);
// #endif
//                     DBP("OffloadingDecision: MyLoad: %d, Rank %d is empty, LastKnownSumOutstandingJobs: %d\n", cur_load, tmp_idx, last_known_sum_outstanding);
//                     OffloadEntryTy * off_entry = new OffloadEntryTy(cur_task, tmp_idx);
//                     offload_task_to_rank(off_entry);
//                     offload_triggered = 1;
// #if OFFLOAD_BLOCKING
//                     _offload_blocked = 1;
// #endif
//                     break;
//                 }
//             }

            // only proceed if offloading not already performed
            if(!offload_triggered) {
                // reset values to zero
                std::fill(tasksToOffload.begin(), tasksToOffload.end(), 0);
#if CHAMELEON_TOOL_SUPPORT
            if(cham_t_status.enabled && cham_t_status.cham_t_callback_compute_num_task_to_offload) {
                cham_t_status.cham_t_callback_compute_num_task_to_offload(&(tasksToOffload[0]), &(_load_info_ranks[0]));
            } else {
                computeNumTasksToOffload( tasksToOffload, _load_info_ranks );
            }
#else 
            computeNumTasksToOffload( tasksToOffload, _load_info_ranks );
#endif
                for(int r=0; r<tasksToOffload.size(); r++) { 
                    if(r != chameleon_comm_rank) {
                        int targetOffloadedTasks = tasksToOffload[r];
                        for(int t=0; t<targetOffloadedTasks; t++) {
                            TargetTaskEntryTy *cur_task = _local_tasks.pop_front();
                            if(cur_task) {
#ifdef TRACE
                                VT_end(event_offload_decision);
#endif
                                DBP("OffloadingDecision: MyLoad: %d, Load Rank %d is %d, LastKnownSumOutstandingJobs: %d\n", cur_load, r, _load_info_ranks[r], last_known_sum_outstanding);
                                OffloadEntryTy * off_entry = new OffloadEntryTy(cur_task, r);
                                offload_task_to_rank(off_entry);
                                offload_triggered = 1;
#if OFFLOAD_BLOCKING
                                _offload_blocked = 1;
#endif
                            }
                        }
                    }
                }
            }
#ifdef TRACE
            if(!offload_triggered)
                VT_end(event_offload_decision);
#endif
#if OFFLOAD_BLOCKING
            }
#endif
        }
#endif // #if !FORCE_OFFLOAD_MASTER_WORKER && OFFLOAD_ENABLED

        // ================= Sending back results for stolen tasks =================
        if(_stolen_remote_tasks_send_back.empty()) {
            usleep(CHAM_SPEEL_TIME_MICRO_SECS);
            continue;
        }

        cur_task = _stolen_remote_tasks_send_back.pop_front();
        // need to check again
        if(!cur_task) {
            continue;
        }

#ifdef TRACE
        VT_begin(event_send_back);
#endif
        DBP("service_thread_action - sending back data to rank %d with tag %d for (task_id=%d)\n", cur_task->source_mpi_rank, cur_task->source_mpi_tag, cur_task->task_id);

#if OFFLOAD_DATA_PACKING_TYPE == 0
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
                print_arg_info("service_thread_action", cur_task, i);
                memcpy(cur_ptr, cur_task->arg_hst_pointers[i], cur_task->arg_sizes[i]);
                cur_ptr += cur_task->arg_sizes[i];
            }
        }

        // initiate blocking send
#if CHAM_STATS_RECORD
        double cur_time = omp_get_wtime();
#endif
        MPI_Request request;
        MPI_Isend(buff, tmp_size_buff, MPI_BYTE, cur_task->source_mpi_rank, cur_task->source_mpi_tag, chameleon_comm_mapped, &request);
#if CHAM_STATS_RECORD
        cur_time = omp_get_wtime()-cur_time;
        atomic_add_dbl(_time_comm_back_send_sum, cur_time);
        _time_comm_back_send_count++;
#endif
        request_manager_send.submitRequests( cur_task->source_mpi_tag, cur_task->source_mpi_rank, 1, &request, MPI_BLOCKING, send_back_handler, sendBack, buff );
#elif OFFLOAD_DATA_PACKING_TYPE == 1

#if CHAM_STATS_RECORD
        double cur_time = omp_get_wtime();
#endif
        MPI_Request *requests = new MPI_Request[cur_task->arg_num];
        int j = 0;        

        for(int i = 0; i < cur_task->arg_num; i++) {
            if(cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                MPI_Isend(cur_task->arg_hst_pointers[i], cur_task->arg_sizes[i], MPI_BYTE, cur_task->source_mpi_rank, cur_task->source_mpi_tag, chameleon_comm_mapped, &requests[j++]);
            }
        }
        request_manager_send.submitRequests( cur_task->source_mpi_tag, cur_task->source_mpi_rank, j, &requests[0], MPI_BLOCKING, send_back_handler, sendBack, nullptr );
        delete[] requests;
#if CHAM_STATS_RECORD
        cur_time = omp_get_wtime()-cur_time;
        atomic_add_dbl(_time_comm_back_send_sum, cur_time);
        _time_comm_back_send_count++;
#endif

#endif

#ifdef TRACE
        VT_end(event_send_back);
#endif
        _mtx_load_exchange.lock();
        _num_stolen_tasks_outstanding--;
        trigger_update_outstanding();
        _mtx_load_exchange.unlock();
    }
}
#pragma endregion Thread Send / Service

#pragma region Helper Functions
int exit_condition_met(int print) {
    if( _num_threads_entered_taskwait >= _num_threads_involved_in_taskwait && _num_threads_idle >= _num_threads_involved_in_taskwait) {
        int cp_ranks_not_completely_idle = _num_ranks_not_completely_idle;
        if( _comm_thread_load_exchange_happend && _outstanding_jobs_sum == 0 && cp_ranks_not_completely_idle == 0) {
            if(print)
                DBP("exit_condition_met - _num_threads_entered_taskwait: %d exchange_happend: %d oustanding: %d _num_ranks_not_completely_idle: %d\n", _num_threads_entered_taskwait.load(), _comm_thread_load_exchange_happend, _outstanding_jobs_sum.load(), cp_ranks_not_completely_idle);
            return 1;
        }
    }
    return 0;
}

void trigger_update_outstanding() {
    _outstanding_jobs_local = _num_local_tasks_outstanding + _num_stolen_tasks_outstanding;
    DBP("trigger_update_outstanding - current outstanding jobs: %d, current_local_load = %d\n", _outstanding_jobs_local.load(), _load_info_local);
}

void print_arg_info(std::string prefix, TargetTaskEntryTy *task, int idx) {
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

void print_arg_info_w_tgt(std::string prefix, TargetTaskEntryTy *task, int idx) {
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

#ifdef __cplusplus
}
#endif
