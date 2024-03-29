#include "commthread.h"
#include "chameleon_common.h"
#include "chameleon_statistics.h"
#include "chameleon_strategies.h"
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
#include <string>
#include <iostream>

#ifdef TRACE
#include "VT.h"
#endif

#pragma region Variables
// communicator for remote task requests
MPI_Comm chameleon_comm;
// communicator for sending back mapped values
MPI_Comm chameleon_comm_mapped;
// communicator for cancelling offloaded tasks
MPI_Comm chameleon_comm_cancel;
// communicator for load information
MPI_Comm chameleon_comm_load;
// communicator for activating replicated tasks
MPI_Comm chameleon_comm_activate;


int chameleon_comm_rank = -1;
int chameleon_comm_size = -1;

// original cpuset of the complete process 
// (needs to be recorded in serial region at the beginning of the applicatrion)
cpu_set_t pid_mask;

//request manager for MPI requests
RequestManager request_manager_receive;
RequestManager request_manager_send;
RequestManager request_manager_cancel;

// Communication tracking
std::atomic<int32_t> _num_active_communications_overall(0);

// array that holds image base addresses
std::vector<intptr_t> _image_base_addresses;

// list with local task entries
// these can either be executed here or offloaded to a different rank (i.e., become replicated tasks)
thread_safe_task_list_t _local_tasks;
std::atomic<int32_t> _num_local_tasks_outstanding(0);

// list with stolen task entries that should be executed but may be cancelled if task is executed on origin rank
thread_safe_task_list_t _stolen_remote_tasks;
std::atomic<int32_t> _num_remote_tasks_outstanding(0);

// list with local replicated (i.e. offloaded) task entries
// these can either be executed remotely or locally
thread_safe_task_list_t _replicated_local_tasks;
std::atomic<int32_t> _num_replicated_local_tasks_outstanding_sends(0);
std::atomic<int32_t> _num_replicated_local_tasks_outstanding_compute(0);

thread_safe_task_list_t _replicated_remote_tasks;
thread_safe_task_list_t _replicated_migrated_tasks;
std::atomic<int32_t> _num_replicated_remote_tasks_outstanding(0);
std::atomic<int32_t> _num_replicated_and_migrated_remote_tasks_outstanding(0);

// list with stolen task entries that need output data transfer
thread_safe_task_list_t _remote_tasks_send_back;

thread_safe_task_list_t _tasks_to_deallocate;
// list with local replicated task entries that need initial transfer
thread_safe_list_t<cham_t_replication_info_t*> _replication_infos_list;

std::vector<int> _num_replicated_local_tasks_per_victim;
std::unordered_set<int> _active_replication_victims;

// map that maps tag ids back to local tasks that have been offloaded and expect result data
thread_safe_task_map_t _map_offloaded_tasks_with_outputs;
// map that maps tag ids back to stolen tasks
thread_safe_task_map_t _map_tag_to_remote_task;
// mapping of all active task ids and task
thread_safe_task_map_t _map_overall_tasks;

std::unordered_set<TYPE_TASK_ID> _cancelled_task_ids;

// ====== Info about outstanding jobs (local & stolen) ======
std::vector<int32_t> _outstanding_jobs_ranks;
std::atomic<int32_t> _outstanding_jobs_local(0);
std::atomic<int32_t> _outstanding_jobs_sum(0);
// counter for current number of offloaded tasks
//std::atomic<int> _num_offloaded_tasks_outstanding(0);

// ====== Info about real load that is open or is beeing processed ======
std::vector<int32_t> _load_info_ranks;
std::mutex _mtx_cancellation;

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
std::atomic<int> _flag_abort_comm_thread(0);
int tag_counter_send_tasks = 0;

// variables to indicate when it is save to break out of taskwait
std::mutex _mtx_taskwait;
std::atomic<int> _flag_comm_thread_sleeping(1);

std::atomic<int> _num_threads_involved_in_taskwait(INT_MAX);
std::atomic<int> _num_threads_active_in_taskwait(0);
std::atomic<int32_t> _num_threads_idle(0);
std::atomic<int> _num_ranks_not_completely_idle(INT_MAX);

// number of active migrations per target rank
// desired: should block new migration to target as long as there are still active migrations ongoing
std::atomic<int> _active_migrations_per_target_rank[1000];

pthread_t           _th_service_actions;
std::atomic<int>    _th_service_actions_created(0);
pthread_cond_t      _th_service_actions_cond    = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     _th_service_actions_mutex   = PTHREAD_MUTEX_INITIALIZER;

// ============== Tracing Section ===========
std::atomic<bool> _trace_events_initialized(false);
int event_receive_tasks          = -1;
int event_recv_back              = -1;
int event_create_gather_request  = -1;
int event_exchange_outstanding   = -1;
int event_offload_decision       = -1;
int event_send_back              = -1;
int event_progress_send          = -1;
int event_progress_recv          = -1;


chameleon_comm_thread_session_data_t _session_data;

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
void offload_action(cham_migratable_task_t **tasks, int32_t num_tasks, int target_rank, bool use_synchronous_mode=false);
int32_t offload_tasks_to_rank(cham_migratable_task_t **tasks, int32_t num_tasks, int target_rank, bool use_synchronus_mode=false);
void add_active_replication_victim(int rank, int ntasks);

static void send_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void receive_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void receive_handler_data(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void send_back_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void receive_back_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
static void receive_back_trash_handler(void* buffer, int tag, int rank, cham_migratable_task_t** tasks, int num_tasks);
inline void action_post_recvback_requests(cham_migratable_task_t *task_entry, int mpi_source, int mpi_tag, RequestManager *request_manager_receive);


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

    DBP("start_communication_threads (enter)\n");
    // set flag to avoid that threads are directly aborting
    _flag_abort_comm_thread = 0;

    // explicitly make thread joinable to be portable
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

    // set flag to ensure that only a single thread is creating communication thread
    _comm_threads_started = 1;
    _mtx_comm_threads_started.unlock();
    DBP("start_communication_threads (exit)\n");
    return CHAM_SUCCESS;
}

int32_t chameleon_wake_up_comm_threads() {
    // if threads already awake
    if(!_flag_comm_thread_sleeping)
        return CHAM_SUCCESS;
    
    _mtx_taskwait.lock();
    // need to check again
    if(!_flag_comm_thread_sleeping) {
        _mtx_taskwait.unlock();
        return CHAM_SUCCESS;
    }

    DBP("chameleon_wake_up_comm_threads (enter) - _flag_comm_thread_sleeping = %d\n", _flag_comm_thread_sleeping.load());
    _flag_comm_thread_sleeping         = 0;
    _mtx_taskwait.unlock();
    DBP("chameleon_wake_up_comm_threads (exit) - _flag_comm_thread_sleeping = %d\n", _flag_comm_thread_sleeping.load());
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
    _flag_abort_comm_thread = 1;
    // then wait for all threads to finish
    err = pthread_join(_th_service_actions, NULL);
    if(err != 0) handle_error_en(err, "stop_communication_threads - _th_service_actions");

    // should be save to reset flags and counters here
    _comm_threads_started = 0;
    #if !THREAD_ACTIVATION
    _comm_threads_ended_count       = 0;
    _th_service_actions_created     = 0;
    #endif
    
    DBP("stop_communication_threads (exit)\n");
    #if !THREAD_ACTIVATION
    _mtx_comm_threads_ended.unlock();
    #endif
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

    DBP("put_comm_threads_to_sleep (enter) - _flag_comm_thread_sleeping = %d\n", _flag_comm_thread_sleeping.load());
    //assert(request_manager_receive.getNumberOfOutstandingRequests()==0);
    //assert(request_manager_send.getNumberOfOutstandingRequests()==0);

    _flag_comm_thread_sleeping             = 1;
    // wait until thread sleeps
    while(!_comm_thread_service_stopped) {
        usleep(COMM_THREAD_SLEEP_TIME_MICRO_SECS);
    }
    // DBP("put_comm_threads_to_sleep - service thread stopped = %d\n", _comm_thread_service_stopped);
    _comm_threads_ended_count               = 0;
    _comm_thread_service_stopped            = 0;

    DBP("put_comm_threads_to_sleep (exit)\n");
    _mtx_comm_threads_ended.unlock();
    return CHAM_SUCCESS;
}

void cleanup_work_phase() {
#if CHAM_REPLICATION_MODE==4
	while(!_replicated_remote_tasks.empty()) {
		cham_migratable_task_t *task = _replicated_remote_tasks.pop_back();
		free_migratable_task(task);
	}

#endif

    while(!_tasks_to_deallocate.empty()) {
        cham_migratable_task_t *task = _tasks_to_deallocate.pop_back();
        free_migratable_task(task);
    }
    _cancelled_task_ids.clear();
    _map_offloaded_tasks_with_outputs.clear();

    for(int i=0; i<_num_replicated_local_tasks_per_victim.size(); i++)
    	_num_replicated_local_tasks_per_victim[i] = 0;

     
    //_active_migrations_per_target_rank.clear();

    _num_replicated_local_tasks_outstanding_compute = 0;
#if CHAM_REPLICATION_MODE==4
    _map_overall_tasks.clear();
#endif

    DBP("cleanup_work_phase - cleared map_offloaded_tasks_with_outputs\n");

    if(_flag_abort_comm_thread) {
        DBP("comm_thread_action (abort)\n");
        if(_session_data.buffer_load_values) {
            free(_session_data.buffer_load_values);
            _session_data.buffer_load_values = nullptr;
        }
    }
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
    cpu_set_t new_cpu_set;

    // this only reflects binding of current thread. Problem when OpenMP already pinned threads due to OMP_PLACES and OMP_PROC_BIND. 
    // then comm thread only get cpuset to single core --> overdecomposition of core with computational and communication thread  
    // err = sched_getaffinity(getpid(), sizeof(cpu_set_t), &pid_mask);
    // if(err != 0) handle_error_en(err, "sched_getaffinity");

    hwloc_topology_t topology;
    err = hwloc_topology_init(&topology);
    err = hwloc_topology_load(topology);

    // ===== DEBUG
    // print_affinity_mask(pid_mask);
    // ===== DEBUG

    // also get the number of processing units (here)
    int depth = hwloc_get_type_depth(topology, HWLOC_OBJ_CORE);
    if(depth == HWLOC_TYPE_DEPTH_UNKNOWN) {
        handle_error_en(1001, "hwloc_get_type_depth");
    }
    const long n_physical_cores = hwloc_get_nbobjs_by_depth(topology, depth);
    const long n_logical_cores = sysconf( _SC_NPROCESSORS_ONLN );
    int ht_enabled = n_logical_cores > n_physical_cores;
    hwloc_topology_destroy(topology);
    
    // get last hw thread of current cpuset
    long max_core_set = -1;
    int count_last = 0;

    for (long i = n_logical_cores-1; i >= 0; i--) {
        //RELP("core/hw thread %ld not set in cpuset\n", i);
        if (CPU_ISSET(i, &pid_mask)) {
            // RELP("Last core/hw thread in cpuset is %ld\n", i);
            max_core_set = i;
            count_last++;
            if(count_last >= n_last_core)
                break;
        }
    }

    // set affinity mask to last core or all hw threads on specific core 
    CPU_ZERO(&new_cpu_set);
    if(!ht_enabled) {
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
    // cpu_set_t final_cpu_set;
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
	DBP("send_handler invoked\n");
    free(buffer);
    _active_migrations_per_target_rank[source]--;
    for(int i_task = 0; i_task<num_tasks; i_task++) {
    	cham_migratable_task_t *task = tasks[i_task];
    	if(task->is_replicated_task) {
        _num_replicated_local_tasks_outstanding_sends--;
    	  task->num_outstanding_replication_sends--;
    	  DBP("send_handler, replicated task, outstanding sends %d\n", task->num_outstanding_replication_sends);
          // now we can push the replicated task into the local queue as it can safely be executed
    	  if(!task->num_outstanding_replication_sends) {
    		  DBP("send_handler, pushing replicated task into replicated local queue, task_id: %d\n", task->task_id);
    		  _replicated_local_tasks.push_back(task);
    	  }
    	}
    }
    free(tasks);
};

static void receive_handler_data(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    // add tasks to stolen list
    for (int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];
        // check if task has already been cancelled
        if(_cancelled_task_ids.find(task->task_id)!=_cancelled_task_ids.end()) {
            _cancelled_task_ids.erase(task->task_id);
            if(task->is_migrated_task)
              _num_replicated_and_migrated_remote_tasks_outstanding--;
            free_migratable_task(task, 1);
            _num_replicated_remote_tasks_outstanding--;
            _num_remote_tasks_outstanding--;
            DBP("receive_handler_data - late cancel, decrement stolen outstanding for task id: %d  new count %d\n", task->task_id, _num_remote_tasks_outstanding.load());
            DBP("receive_handler_data - conducted late cancel for task id: %d\n", task->task_id);

            #if CHAM_STATS_RECORD
            _num_tasks_canceled++;
            #endif
        	continue;
        }

        if(task->is_replicated_task && !task->is_migrated_task)
           _replicated_remote_tasks.push_back(task);
        else
#if  REPLICATION_PRIORITIZE_MIGRATED==0 && CHAM_REPLICATION_MODE==3
           _replicated_remote_tasks.push_back(task);
#elif REPLICATION_PRIORITIZE_MIGRATED==1 && CHAM_REPLICATION_MODE==3
           _replicated_migrated_tasks.push_back(task);
#else
           _stolen_remote_tasks.push_back(task);
#endif
        _map_tag_to_remote_task.insert(task->task_id, task);
        _map_overall_tasks.insert(task->task_id, task);
    }
    // free memory again here
    free(tasks);
}

static void receive_handler(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    DBP("receive_handler - received tasks from rank %d with tag: %d\n", source, tag);
    std::vector<cham_migratable_task_t*> list_tasks;
    int32_t n_tasks = 0;
#if CHAM_STATS_RECORD
    double cur_time_decode, cur_time;
    cur_time_decode = omp_get_wtime();
#endif
    int num_bytes_received = 0;
    decode_send_buffer(buffer, tag, &n_tasks, list_tasks);
    free(buffer);
#if CHAM_STATS_RECORD
    cur_time_decode = omp_get_wtime()-cur_time_decode;
    atomic_add_dbl(_time_decode_sum, cur_time_decode);
    _time_decode_count++;
#endif

    // add stolen load as soon as possible to avoid wrong decision making
#if CHAM_REPLICATION_MODE!=4
    _num_remote_tasks_outstanding += n_tasks;
    DBP("receive_handler - increment stolen outstanding count for tag %d by %d, new count %d\n", tag, n_tasks, _num_remote_tasks_outstanding.load());
#endif

    // set mpi source + copy pointers to separate array
    cham_migratable_task_t** p_tasks = (cham_migratable_task_t**) malloc(n_tasks*sizeof(cham_migratable_task_t*));
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        list_tasks[i_task]->source_mpi_rank = source;
        p_tasks[i_task] = list_tasks[i_task];
#if CHAM_REPLICATION_MODE!=4
        if(p_tasks[i_task]->is_replicated_task)
          _num_replicated_remote_tasks_outstanding++;
        if(p_tasks[i_task]->is_migrated_task)
          _num_replicated_and_migrated_remote_tasks_outstanding++;       

#else // CHAM_REPLICATION_MODE==4
        if(p_tasks[i_task]->is_migrated_task) {
            DBP("receive_handler - increment stolen outstanding count for tag %d by %d, new count %d\n", tag, n_tasks, _num_remote_tasks_outstanding.load());
           _num_remote_tasks_outstanding++;
           _session_data.is_migration_victim = true;
        }
#endif
    }
#if OFFLOAD_DATA_PACKING_TYPE == 0
    receive_handler_data(NULL, tag, source, p_tasks, n_tasks);
#elif OFFLOAD_DATA_PACKING_TYPE > 0
    double start_time_requests = 0;
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime();
#endif
#if OFFLOAD_DATA_PACKING_TYPE == 1
    int num_requests = 0;
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        for(int i_par = 0; i_par < list_tasks[i_task]->arg_num; i_par++) {
            if(list_tasks[i_task]->arg_types[i_par] & CHAM_OMP_TGT_MAPTYPE_TO)
                num_requests++;
        }
    }
    MPI_Request *requests = new MPI_Request[num_requests];
    int cur_req_num = 0;

    #if CHAM_STATS_RECORD
    start_time_requests = omp_get_wtime();
    #endif
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        cham_migratable_task_t *task = list_tasks[i_task];
        for(int i=0; i<task->arg_num; i++) {
            int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_to       = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_TO;

            if(is_to) {
                if(is_lit) {
                    #if MPI_BLOCKING
                    int ierr = MPI_Recv(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, source, tag, chameleon_comm, MPI_STATUS_IGNORE);
                    #else
                    int ierr = MPI_Irecv(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, source, tag, chameleon_comm, &requests[cur_req_num]);
                    #endif
                    assert(ierr==MPI_SUCCESS);
                } else {
                    #if MPI_BLOCKING
                    int ierr = MPI_Recv(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, source, tag, chameleon_comm, MPI_STATUS_IGNORE);
                    #else
                    int ierr = MPI_Irecv(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, source, tag, chameleon_comm, &requests[cur_req_num]);
                    #endif
                    assert(ierr==MPI_SUCCESS);
                }
                #if CHAM_STATS_RECORD
                num_bytes_received += task->arg_sizes[i];
                _stats_bytes_recv_per_message.add_stat_value((double)task->arg_sizes[i]);
                #endif
                cur_req_num++;
                print_arg_info("receive_handler - receiving argument", task, i);
            }
        }
    }
#elif OFFLOAD_DATA_PACKING_TYPE == 2
    int num_requests = 1;
    MPI_Request *requests = new MPI_Request[num_requests];
    int tmp_overall_arg_nums = 0;
    for(int i_task = 0; i_task < n_tasks; i_task++) {
        for(int i_par = 0; i_par < list_tasks[i_task]->arg_num; i_par++) {
            if(list_tasks[i_task]->arg_types[i_par] & CHAM_OMP_TGT_MAPTYPE_TO)
                tmp_overall_arg_nums ++;
        }
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
            if(task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_TO) {
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
    }
    ierr = MPI_Type_create_struct(tmp_overall_arg_nums, blocklen, disp, separate_types, &type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_commit(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    #if CHAM_STATS_RECORD
    int size = 0;
    MPI_Type_size(type_mapped_vars, &size);
    num_bytes_received += size;
    _stats_bytes_recv_per_message.add_stat_value((double)size);
    #endif
    #if MPI_BLOCKING
    ierr = MPI_Recv(MPI_BOTTOM, 1, type_mapped_vars, source, tag, chameleon_comm, MPI_STATUS_IGNORE);
    #else
    #if CHAM_STATS_RECORD
    start_time_requests = omp_get_wtime();
    #endif
    ierr = MPI_Irecv(MPI_BOTTOM, 1, type_mapped_vars, source, tag, chameleon_comm, &requests[0]);
    #endif
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_free(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
#endif /* OFFLOAD_DATA_PACKING_TYPE == ... */
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    #if MPI_BLOCKING
    add_throughput_recv(cur_time, num_bytes_received);
    #endif
#endif /* CHAM_STATS_RECORD */

    #if MPI_BLOCKING
    receive_handler_data(NULL, tag, source, p_tasks, n_tasks);
    #else
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = receive_handler_data;
    request_manager_receive.submitRequests(start_time_requests, tag, 
                                    source,
                                    num_requests, 
                                    requests,
                                    num_bytes_received,
                                    0,
                                    cur_handler,
                                    recvData, 
                                    NULL, 
                                    p_tasks,
                                    n_tasks);
    #endif
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
#if CHAM_REPLICATION_MODE!=4
            if(task->is_replicated_task)
            	_num_replicated_remote_tasks_outstanding--;
            if(task->is_migrated_task)
              _num_replicated_and_migrated_remote_tasks_outstanding--;
#endif
            _num_remote_tasks_outstanding--;
            DBP("send_back_stolen_tasks - decrement stolen outstanding count for task %ld new count: %ld\n", task->task_id, _num_remote_tasks_outstanding.load());
            free_migratable_task(task, true);
        }
        free(tasks);
    }
}

static void receive_back_handler(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    DBP("receive_back_handler - receiving output data from rank %d for tag: %d\n", source, tag);
    cham_migratable_task_t *task_entry = _map_offloaded_tasks_with_outputs.find(tag);
    assert(task_entry!=nullptr);
    if(task_entry) {
        assert(task_entry->num_outstanding_recvbacks>=0);
       // if(task_entry->num_outstanding_recvbacks==0)
       // 	_map_offloaded_tasks_with_outputs.erase(tag);

        #if CHAM_REPLICATION_MODE>=2
        if(task_entry->is_replicated_task) {
        	for(auto rank : task_entry->replication_ranks) {
        		if(rank!=source)
        			cancel_offloaded_task_on_rank(task_entry, rank);
        	}
        }
        #endif

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
        //_num_offloaded_tasks_outstanding--;

        // mark locally created task finished
        #if CHAMELEON_ENABLE_FINISHED_TASK_TRACKING
        _unfinished_locally_created_tasks.remove(task_entry->task_id);
        #endif
        _map_overall_tasks.erase(task_entry->task_id);
        _num_local_tasks_outstanding--;
        assert(_num_local_tasks_outstanding>=0);
        DBP("receive_back_handler -  local outstanding count for task %ld, new count %d\n", task_entry->task_id, _num_local_tasks_outstanding.load());

        // handle external finish callback
        if(task_entry->cb_task_finish_func_ptr) {
            task_entry->cb_task_finish_func_ptr(task_entry->cb_task_finish_func_param);
        }
        //if(task_entry->num_outstanding_recvbacks==0)
        //  free_migratable_task(task_entry, false);
    }

    if(tasks)
        free(tasks);
}

static void receive_back_trash_handler(void* buffer, int tag, int source, cham_migratable_task_t** tasks, int num_tasks) {
    DBP("receive_remote_tasks_trash - receiving output data from rank %d for tag into trash: %d num_tasks: %d\n", source, tag, num_tasks);
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];
        DBP("receive_remote_tasks_trash - outstanding: %d for task id: %d\n",  task->num_outstanding_recvbacks, task->task_id);
        
        if(task->num_outstanding_recvbacks==0) {
            //_map_offloaded_tasks_with_outputs.erase(tag);
            // mark locally created task finished
            _unfinished_locally_created_tasks.remove(task->task_id);
            _map_overall_tasks.erase(task->task_id);

            // handle external finish callback
            if(task->cb_task_finish_func_ptr) {
                task->cb_task_finish_func_ptr(task->cb_task_finish_func_param);
            }

            if(!task->is_replicated_task)
                free_migratable_task(task, false);
        }
    }
    if(buffer)
        free(buffer);
    free(tasks);
}
#pragma endregion

#pragma region Offloading / Packing


void add_active_replication_victim(int rank, int ntasks) {
	DBP("add_active_replication_victim - rank %d, ntasks %d\n", rank, ntasks);
	_active_replication_victims.insert(rank);
	_num_replicated_local_tasks_per_victim[rank]+= ntasks;
}

int32_t activate_remote_replicated_tasks(int rank, int ntasks) {
        int cnt = 0;
	cham_migratable_task_t *task = nullptr;
	DBP("activate_remote_replicated_tasks - trying to activate %d tasks from rank %d\n", ntasks, rank);

	while(cnt<ntasks && !_replicated_remote_tasks.empty()) {
		task = _replicated_remote_tasks.pop_task_by_rank(rank);
       	_num_remote_tasks_outstanding++;
		_stolen_remote_tasks.push_back(task);
		cnt++;
	}

	DBP("activated %d tasks\n", cnt);
	return CHAM_SUCCESS;
}

void cancel_offloaded_task(cham_migratable_task_t *task) {
    DBP("cancel_offloaded_task - canceling offloaded task, task_id: %ld on remote ranks\n", task->task_id);

    task->is_cancelled = 1;
    // we have sent the task already
    if(task->num_outstanding_recvbacks) {
      //if(task->target_mpi_rank>=0) {
      //  cancel_offloaded_task_on_rank(task, task->target_mpi_rank);
      //}
      for( auto rank : task->replication_ranks ) {
        cancel_offloaded_task_on_rank(task, rank);
      }
    }
}

void cancel_offloaded_task_on_rank(cham_migratable_task_t *task, int rank) {
    assert(rank>=0);
    assert(rank<chameleon_comm_size);
    
    DBP("cancel_offloaded_task - canceling offloaded task, task_id: %ld on remote rank: %d\n", task->task_id, rank);
    //printf("cancel_offloaded_task - canceling offloaded task, task_id: %ld on remote rank: %d\n", task->task_id, rank);

    MPI_Request request; 
    // TODO: depends on int32 or int64
    double start_time_requests = 0;
    #if CHAM_STATS_RECORD
    start_time_requests = omp_get_wtime();
    #endif
    int ierr= MPI_Isend(&task->task_id, 1, MPI_INTEGER, rank, 0, chameleon_comm_cancel, &request);
    assert(ierr==MPI_SUCCESS);
    //printf("cancel_offloaded_task - posted request %ld\n", request);
    _mtx_cancellation.lock();
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = handler_noop;
    request_manager_cancel.submitRequests( start_time_requests, 0, rank, 1,
                                           &request,
                                           -1, // TODO: what will this be?
                                           0, 
                                           cur_handler,
                                           send,   // TODO: special request
                                           nullptr);
    _mtx_cancellation.unlock();
}

void activate_replicated_tasks_on_rank(int ntasks, int rank) {

    DBP("activate_replicated_tasks_on_rank - sending activation message to rank %d\n", rank);
	int ierr = MPI_Send(&ntasks, 1, MPI_INTEGER, rank, 0, chameleon_comm_activate);
	int tmp = _num_replicated_local_tasks_outstanding_compute - ntasks;
	_num_replicated_local_tasks_outstanding_compute = std::max(tmp, 0);
}


int32_t offload_tasks_to_rank(cham_migratable_task_t **tasks, int32_t num_tasks, int target_rank, bool use_synchronous_mode) {
    #ifdef TRACE
    static int event_offload = -1;
    std::string event_offload_name = "offload_tasks";
    if(event_offload == -1) 
        int ierr = VT_funcdef(event_offload_name.c_str(), VT_NOCLASS, &event_offload);
    VT_BEGIN_CONSTRAINED(event_offload);
    #endif /* TRACE */

    //assert(tasks[0]->result_in_progress.load()==false);

    #if CHAM_DEBUG
    std::string str_task_ids = std::to_string(tasks[0]->task_id);
    for(int i_task = 1; i_task < num_tasks; i_task++) {
        //assert(tasks[i_task]->result_in_progress.load()==false);
        str_task_ids.append("," + std::to_string(tasks[i_task]->task_id));
    }
    DBP("offload_tasks_to_rank (enter) - num_tasks: %d, target_rank: %d, task_ids: %s\n", num_tasks, target_rank, str_task_ids.c_str());
    #endif

    
    offload_action(tasks, num_tasks, target_rank, use_synchronous_mode);
    //_num_offloaded_tasks_outstanding += num_tasks;

    #if CHAM_STATS_RECORD
    _num_tasks_offloaded += num_tasks;
    #endif

    DBP("offload_tasks_to_rank (exit)\n");
    
    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_offload);
    #endif
    return CHAM_SUCCESS;
}

void offload_action(cham_migratable_task_t **tasks, int32_t num_tasks, int target_rank, bool use_synchronous_mode) {
    // use unique tag for current offload in this sync cycle
    int tmp_tag = tag_counter_send_tasks++;
    DBP("offload_action (enter) - num_tasks: %d, target_rank: %d mpi_tag: %d\n", num_tasks, target_rank, tmp_tag);

    // store target rank in task
    for(int i = 0; i < num_tasks; i++) {
      if(!tasks[i]->is_replicated_task) {
    	DBP("offloading_action - offloading migrated task\n"); assert(tasks[i]->is_migrated_task);
    	//printf("offloading_action - offloading migrated task to %d\n", tasks[i]->target_mpi_rank); assert(tasks[i]->is_migrated_task);
        tasks[i]->target_mpi_rank = target_rank;
      }
      else {
    	  DBP("offload_action - offloading replicated task\n");
          #if CHAM_STATS_RECORD
          _num_tasks_replicated += 1;
          #endif
    	  //printf("offload_action - offloading replicated task to %d, is_migrated %d\n", target_rank, tasks[i]->is_migrated_task);
    	  _num_replicated_local_tasks_outstanding_sends += tasks[i]->replication_ranks.size();
    	  tasks[i]->num_outstanding_replication_sends = tasks[i]->replication_ranks.size();
    	  if(!tasks[i]->is_migrated_task)
    	    _num_replicated_local_tasks_outstanding_compute++; //we'll likely need to compute this on our own
      }
    }

    // encode buffer
    int32_t buffer_size = 0;
    void *buffer = NULL;
    int num_bytes_sent = 0;
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

    double start_time_requests = 0;
#if OFFLOAD_DATA_PACKING_TYPE == 0
    // RELP("Packing Type: Buffer\n");
    int n_requests = 1;
#elif OFFLOAD_DATA_PACKING_TYPE == 1
    // RELP("Packing Type: Zero Copy\n");
    int n_requests = 1;
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        for(int tmp_i_arg = 0; tmp_i_arg < tasks[i_task]->arg_num; tmp_i_arg++) {
            int is_to = tasks[i_task]->arg_types[tmp_i_arg] & CHAM_OMP_TGT_MAPTYPE_TO;
            if(is_to)
                n_requests++;
        }
    }
#elif OFFLOAD_DATA_PACKING_TYPE == 2
    // RELP("Packing Type: Zero Copy Single Message\n");
    int n_requests = 2;
#endif

    // send data to target rank
    DBP("offload_action - sending data to target rank %d with mpi_tag: %d\n", target_rank, tmp_tag);
#if CHAM_STATS_RECORD
    num_bytes_sent += buffer_size;
    // #if OFFLOAD_DATA_PACKING_TYPE == 0
    _stats_bytes_send_per_message.add_stat_value((double)buffer_size);
    // #endif
    start_time_requests = cur_time = omp_get_wtime();
#endif
    MPI_Request *requests = new MPI_Request[n_requests];
    #if MPI_BLOCKING
    MPI_Send(buffer, buffer_size, MPI_BYTE, target_rank, tmp_tag, chameleon_comm);
    #else
    if(use_synchronous_mode)
        MPI_Issend(buffer, buffer_size, MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[0]);
    else
        MPI_Isend(buffer, buffer_size, MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[0]);
    #endif
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    #if MPI_BLOCKING
    add_throughput_send(cur_time, buffer_size);
    #endif
#endif

#if OFFLOAD_DATA_PACKING_TYPE > 0 && CHAM_STATS_RECORD
    cur_time = omp_get_wtime();
#endif

#if OFFLOAD_DATA_PACKING_TYPE == 1
    int cur_req_index = 1;
#if CHAM_STATS_RECORD
    int tmp_bytes_send = 0;
#endif
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];
        for(int i=0; i<task->arg_num; i++) {
            int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_to       = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_TO;
            if(is_to) {
                if(is_lit) {
                    #if MPI_BLOCKING
                    MPI_Send(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, target_rank, tmp_tag, chameleon_comm);
                    #else
                    if(use_synchronous_mode)
                        MPI_Issend(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[cur_req_index]);
                    else
                        MPI_Isend(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[cur_req_index]);
                    #endif                
                }
                else {
                    #if MPI_BLOCKING
                    MPI_Send(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, target_rank, tmp_tag, chameleon_comm);
                    #else
                    if(use_synchronous_mode)
                        MPI_Issend(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[cur_req_index]);
                    else
                        MPI_Isend(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, target_rank, tmp_tag, chameleon_comm, &requests[cur_req_index]);
                    #endif
                }
#if CHAM_STATS_RECORD
                tmp_bytes_send += task->arg_sizes[i];
                _stats_bytes_send_per_message.add_stat_value((double)task->arg_sizes[i]);
#endif
                cur_req_index++;
                print_arg_info("offload_action - sending argument", task, i);
            }
        }
    }
#if CHAM_STATS_RECORD
    num_bytes_sent += tmp_bytes_send;
    cur_time = omp_get_wtime()-cur_time;
    #if MPI_BLOCKING
    add_throughput_send(cur_time, tmp_bytes_send);
    #endif
#endif
#elif OFFLOAD_DATA_PACKING_TYPE == 2
    int tmp_overall_arg_nums = 0;
    for(int i_task = 0; i_task < num_tasks; i_task++) {
        for(int tmp_i_arg = 0; tmp_i_arg < tasks[i_task]->arg_num; tmp_i_arg++) {
            int is_to = tasks[i_task]->arg_types[tmp_i_arg] & CHAM_OMP_TGT_MAPTYPE_TO;
            if(is_to)
                tmp_overall_arg_nums++;
        }
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
            int is_to                       = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_TO;
            if(is_to) {
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
    }
    ierr = MPI_Type_create_struct(tmp_overall_arg_nums, blocklen, disp, separate_types, &type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_commit(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
#if CHAM_STATS_RECORD
    int size = 0;
    MPI_Type_size(type_mapped_vars, &size);
    num_bytes_sent += size;
    _stats_bytes_send_per_message.add_stat_value((double)size);
#endif
    #if MPI_BLOCKING
    ierr = MPI_Send(MPI_BOTTOM, 1, type_mapped_vars, target_rank, tmp_tag, chameleon_comm);
    #else
    if( use_synchronous_mode )
      ierr = MPI_Issend(MPI_BOTTOM, 1, type_mapped_vars, target_rank, tmp_tag, chameleon_comm, &requests[1]);
    else
      ierr = MPI_Isend(MPI_BOTTOM, 1, type_mapped_vars, target_rank, tmp_tag, chameleon_comm, &requests[1]);
    #endif
    assert(ierr==MPI_SUCCESS);
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    #if MPI_BLOCKING
    add_throughput_send(cur_time, size);
    #endif
#endif
    ierr = MPI_Type_free(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
#endif /* OFFLOAD_DATA_PACKING_TYPE */

    for(int i_task = 0; i_task < num_tasks; i_task++) {
        cham_migratable_task_t *task = tasks[i_task];
        if(task->HasAtLeastOneOutput()) {
            _tasks_to_deallocate.push_back(task);
            _map_offloaded_tasks_with_outputs.insert(task->task_id, task);
            DBP("offload_action - inserted task with id %ld and pointer %p into offloaded map with outputs\n", task->task_id, task);
            assert(task->num_outstanding_recvbacks>=0);
            task->num_outstanding_recvbacks++;
            DBP("offload_action - increment outstanding recvbacks for task with id %ld new count: %d\n", task->task_id, task->num_outstanding_recvbacks);
            assert(task->num_outstanding_recvbacks>0);

            // early irecv here
            #if CHAM_REPLICATION_MODE==0 && ENABLE_EARLY_IRECVS==1
            if(!task->is_replicated_task) {
                action_post_recvback_requests(task, target_rank, task->task_id, &request_manager_receive);
            }
            #endif
        }
    }
    _active_migrations_per_target_rank[target_rank]++;
    #if MPI_BLOCKING
    send_handler(buffer, tmp_tag, target_rank, nullptr, 0);
    #else
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = send_handler;
    request_manager_send.submitRequests(start_time_requests, tmp_tag, target_rank, n_requests, 
                                requests,
                                num_bytes_sent,
                                0,
                                cur_handler,
                                send,
                                buffer,
                                tasks,
                                num_tasks);
    #endif
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
    //      5. is_replicated
    //      6. is_replicated
    //      7. number of arguments = int32_t
    //      8. array with argument sizes = n_args * int64_t
    //      9. array with argument offsets = n_args * int64_t
    //      10. array with argument types = n_args * int64_t
    //      11. array with values (only for OFFLOAD_DATA_PACKING_TYPE == 0 )
    //      12. annotations
    //      13. tool data if available

    int total_size = sizeof(int32_t);               // 0. number of tasks

    for (int i = 0; i < num_tasks; i++) {
        total_size += sizeof(intptr_t)                  // 1. target entry pointer
            + sizeof(int32_t)                           // 2. img index
            + sizeof(ptrdiff_t)                         // 3. offset inside image
            + sizeof(TYPE_TASK_ID)                      // 4. task id
            + sizeof(int32_t)                           // 5. is_replicated
            + sizeof(int32_t)                           // 6. is_migrated
            + sizeof(int32_t)                           // 7. number of arguments
            + tasks[i]->arg_num * sizeof(int64_t)       // 8. argument sizes
            + tasks[i]->arg_num * sizeof(ptrdiff_t)     // 9. offsets
            + tasks[i]->arg_num * sizeof(int64_t);      // 10. argument types

        #if OFFLOAD_DATA_PACKING_TYPE == 0
        tasks[i]->buffer_size_output_data = 0;
        for(int i_arg = 0; i_arg < tasks[i]->arg_num; i_arg++) {
            int is_to       = tasks[i]->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_TO;
            int is_from     = tasks[i]->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_FROM;
            if(is_to)
                total_size += tasks[i]->arg_sizes[i_arg];
            if(is_from)
                tasks[i]->buffer_size_output_data += tasks[i]->arg_sizes[i_arg];
        }
        #endif /* OFFLOAD_DATA_PACKING_TYPE */
    }

    #if CHAM_MIGRATE_ANNOTATIONS
    std::vector<int32_t> annotation_sizes(num_tasks);
    std::vector<void*> annotation_buffers(num_tasks);
    for (int i = 0; i < num_tasks; i++) {
        int32_t task_annotations_buf_size = 0;
        void *task_annotations_buffer = nullptr;
        total_size += sizeof(int32_t);
        if(tasks[i]->task_annotations) {
            task_annotations_buffer = tasks[i]->task_annotations->pack(&task_annotations_buf_size);
            total_size += task_annotations_buf_size; // size information + buffer size
        }
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

        // 5. replication info
        ((int32_t *) cur_ptr)[0] = tasks[i]->is_replicated_task;
        cur_ptr += sizeof(int32_t);

        // 6. is_migrated
        ((int32_t *) cur_ptr)[0] = tasks[i]->is_migrated_task;
        cur_ptr += sizeof(int32_t);

        // 7. number of arguments
        ((int32_t *) cur_ptr)[0] = tasks[i]->arg_num;
        cur_ptr += sizeof(int32_t);

        // 8. argument sizes
        memcpy(cur_ptr, &(tasks[i]->arg_sizes[0]), tasks[i]->arg_num * sizeof(int64_t));
        cur_ptr += tasks[i]->arg_num * sizeof(int64_t);

        // 9. offsets
        memcpy(cur_ptr, &(tasks[i]->arg_tgt_offsets[0]), tasks[i]->arg_num * sizeof(ptrdiff_t));
        cur_ptr += tasks[i]->arg_num * sizeof(ptrdiff_t);

        // 10. argument types
        memcpy(cur_ptr, &(tasks[i]->arg_types[0]), tasks[i]->arg_num * sizeof(int64_t));
        cur_ptr += tasks[i]->arg_num * sizeof(int64_t);

        #if OFFLOAD_DATA_PACKING_TYPE == 0
        // 11. loop through arguments and copy values
        for(int32_t i_arg = 0; i_arg < tasks[i]->arg_num; i_arg++) {
            int is_lit      = tasks[i]->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_to       = tasks[i]->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_TO;

            if(is_to) {
                print_arg_info("encode_send_buffer", tasks[i], i_arg);
                // copy value from host pointer directly
                if(is_lit) {
                    ((intptr_t *) cur_ptr)[0] = (intptr_t) tasks[i]->arg_hst_pointers[i_arg];
                } else {
                    memcpy(cur_ptr, (char*)tasks[i]->arg_hst_pointers[i_arg], tasks[i]->arg_sizes[i_arg]);
                }
                cur_ptr += tasks[i]->arg_sizes[i_arg];
            }
        }
        #endif /* OFFLOAD_DATA_PACKING_TYPE */

        #if CHAM_MIGRATE_ANNOTATIONS
        // 12. annotations
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
        // 13. tool data
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
        DBP("decode_send_buffer - created task %p\n", task);
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

        // 5. replication info
        task->is_replicated_task = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);

        // 6. is_migrated
        task->is_migrated_task = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);

        // 7. number of arguments
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
        
        // 8. argument sizes
        memcpy(&(task->arg_sizes[0]), cur_ptr, task->arg_num * sizeof(int64_t));
        cur_ptr += task->arg_num * sizeof(int64_t);

        // 9. argument types
        memcpy(&(task->arg_tgt_offsets[0]), cur_ptr, task->arg_num * sizeof(ptrdiff_t));
        cur_ptr += task->arg_num * sizeof(ptrdiff_t);

        // 10. offsets
        memcpy(&(task->arg_types[0]), cur_ptr, task->arg_num * sizeof(int64_t));
        cur_ptr += task->arg_num * sizeof(int64_t);

        // 11. loop through arguments and copy values
        for(int32_t i_arg = 0; i_arg < task->arg_num; i_arg++) {
            int is_lit      = task->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_to       = task->arg_types[i_arg] & CHAM_OMP_TGT_MAPTYPE_TO;

            #if OFFLOAD_DATA_PACKING_TYPE == 0
            if(is_lit) {
                if(is_to) {
                    // set value directly
                    intptr_t tmp_ptr = ((intptr_t *) cur_ptr)[0];
                    task->arg_hst_pointers[i_arg] = (void *) tmp_ptr;
                }
            } else {
                // need to allocate new memory
                void * new_mem = malloc(task->arg_sizes[i_arg]);
                if(is_to) {
                    // copy data from buffer directly
                    memcpy(new_mem, cur_ptr, task->arg_sizes[i_arg]);
                }
                task->arg_hst_pointers[i_arg] = new_mem;
            }
            if(is_to) {
                // increment pointer
                cur_ptr += task->arg_sizes[i_arg];
            }
            #elif OFFLOAD_DATA_PACKING_TYPE > 0
            // allocate memory for parameter
            if(!is_lit) {
                void * new_mem = malloc(task->arg_sizes[i_arg]);
                task->arg_hst_pointers[i_arg] = new_mem;
            }
            #endif
            print_arg_info("decode_send_buffer", task, i_arg);
        }

        #if CHAM_MIGRATE_ANNOTATIONS
        // 12. task annotations
        int32_t task_annotations_buf_size = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);
        if(task_annotations_buf_size > 0) {
            task->task_annotations = new chameleon_annotations_t();
            task->task_annotations->unpack((void*)cur_ptr);
            cur_ptr += task_annotations_buf_size;
        }
        #endif

        #if CHAMELEON_TOOL_SUPPORT
        // 13. task annotations
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
        if( _comm_thread_load_exchange_happend && _outstanding_jobs_sum.load() == 0 && cp_ranks_not_completely_idle == 0  && request_manager_cancel.getNumberOfOutstandingRequests()==0) {
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
            if( _comm_thread_load_exchange_happend && _outstanding_jobs_sum.load() == 0 && cp_ranks_not_completely_idle == 0 && request_manager_cancel.getNumberOfOutstandingRequests()==0) {
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

void chameleon_comm_thread_session_data_t_init() {
    #ifdef TRACE
    if(!_trace_events_initialized.load()) {
        int ierr = 0;
        
        std::string event_receive_tasks_name = "receive_task";
        ierr = VT_funcdef(event_receive_tasks_name.c_str(), VT_NOCLASS, &event_receive_tasks);

        std::string event_recv_back_name = "receive_back";
        ierr = VT_funcdef(event_recv_back_name.c_str(), VT_NOCLASS, &event_recv_back);

        std::string create_gather_request_name = "create_gather_request";
        ierr = VT_funcdef(create_gather_request_name.c_str(), VT_NOCLASS, &event_create_gather_request);

        std::string exchange_outstanding_name = "exchange_outstanding";
        ierr = VT_funcdef(exchange_outstanding_name.c_str(), VT_NOCLASS, &event_exchange_outstanding);

        std::string event_offload_decision_name = "offload_decision";
        ierr = VT_funcdef(event_offload_decision_name.c_str(), VT_NOCLASS, &event_offload_decision);

        std::string event_send_back_name = "send_back";
        ierr = VT_funcdef(event_send_back_name.c_str(), VT_NOCLASS, &event_send_back);

        std::string event_progress_send_name = "progress_send";
        ierr = VT_funcdef(event_progress_send_name.c_str(), VT_NOCLASS, &event_progress_send);

        std::string event_progress_recv_name = "progress_recv";
        ierr = VT_funcdef(event_progress_recv_name.c_str(), VT_NOCLASS, &event_progress_recv);

        _trace_events_initialized = true;
    }
    #endif

    _session_data.flag_thread_sleeping_set = 0;
    _session_data.num_threads_in_tw = _num_threads_involved_in_taskwait.load();
    _session_data.time_last_load_exchange = 0;
    _session_data.time_gather_posted = 0;
    _session_data.has_replicated = false;
    _session_data.request_gather_created = 0;
    _session_data.offload_triggered = 0;
    _session_data.last_known_sum_outstanding = -1;

    _session_data.tasks_to_offload.resize(chameleon_comm_size);
    _session_data.tracked_last_req_recv.resize(chameleon_comm_size);
    _session_data.buffer_load_values = (int*) malloc(sizeof(int)*3*chameleon_comm_size);
    _session_data.n_task_send_at_once = MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE.load();

    for(int tmp_i = 0; tmp_i < chameleon_comm_size; tmp_i++)
        _session_data.tracked_last_req_recv[tmp_i] = -1;
    
}
#pragma endregion Helper Functions

#pragma region CommThread
inline void action_create_gather_request() {
    int32_t local_load_representation;
    int32_t num_tasks_local = _local_tasks.dup_size();
    int32_t num_tasks_replicated_local = _replicated_local_tasks.dup_size();
    int32_t num_tasks_replicated_remote = _replicated_remote_tasks.dup_size();
    int32_t num_tasks_stolen = _stolen_remote_tasks.dup_size()+ _replicated_migrated_tasks.dup_size();

    assert(num_tasks_local>=0);
    assert(num_tasks_replicated_local>=0);
    assert(num_tasks_replicated_remote>=0);

    TYPE_TASK_ID* ids_local = nullptr;
    TYPE_TASK_ID* ids_stolen = nullptr;
    TYPE_TASK_ID* ids_local_rep = nullptr;
    TYPE_TASK_ID* ids_stolen_rep = nullptr;
    
    #if CHAMELEON_TOOL_SUPPORT
    if(cham_t_status.enabled && cham_t_status.cham_t_callback_determine_local_load) {
        // only get task ids when tool is used since default mode does not require that information
        ids_local   = _local_tasks.get_task_ids(&num_tasks_local);
        ids_stolen  = _stolen_remote_tasks.get_task_ids(&num_tasks_stolen);
        ids_local_rep  = _replicated_local_tasks.get_task_ids(&num_tasks_replicated_local);
        ids_stolen_rep  = _replicated_remote_tasks.get_task_ids(&num_tasks_replicated_remote);
        local_load_representation = cham_t_status.cham_t_callback_determine_local_load(ids_local, num_tasks_local,
        		                                                                       ids_local_rep, num_tasks_replicated_local,
        		                                                                       ids_stolen, num_tasks_stolen,
											       ids_stolen_rep, num_tasks_replicated_remote);
        // clean up again
        free(ids_local);
        free(ids_stolen);
        free(ids_local_rep);
        free(ids_stolen_rep);
    } else {
        // Todo: we need to exclude migrated tasks from load representation, so far this is a hack
        local_load_representation = get_default_load_information_for_rank(ids_local, num_tasks_local, ids_local_rep, _num_replicated_local_tasks_outstanding_compute.load(),
        											      ids_stolen, num_tasks_stolen,
												      ids_stolen_rep, num_tasks_replicated_remote);
    }
    #else 
    // Todo: we need to exclude migrated tasks from load representation, so far this is a hack
    local_load_representation = get_default_load_information_for_rank(ids_local, num_tasks_local, ids_local_rep, _num_replicated_local_tasks_outstanding_compute.load(),
            											  ids_stolen, num_tasks_stolen,
    												  ids_stolen_rep, num_tasks_replicated_remote);
    #endif

    int tmp_val = _num_threads_idle.load() < _session_data.num_threads_in_tw.load() ? 1 : 0;
    // DBP("action_create_gather_request - my current value for rank_not_completely_in_taskwait: %d\n", tmp_val);
    _session_data.transported_load_values[0] = tmp_val;
    // transported_load_values[1] = _outstanding_jobs_local.load();
    _session_data.transported_load_values[1] = _num_local_tasks_outstanding.load() + _num_remote_tasks_outstanding.load() + _num_replicated_local_tasks_outstanding_sends.load() + _num_replicated_remote_tasks_outstanding.load();
    _session_data.transported_load_values[2] = local_load_representation;
        
    MPI_Iallgather(&(_session_data.transported_load_values[0]), 3, MPI_INT, _session_data.buffer_load_values, 3, MPI_INT, chameleon_comm_load, &(_session_data.request_gather_out));
}

inline void action_handle_gather_request() {
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(event_exchange_outstanding);
    #endif
    // sum up that stuff
    // DBP("action_handle_gather_request - gathered new load info\n");
    int32_t old_val_n_ranks                 = _num_ranks_not_completely_idle;
    int32_t old_outstanding_sum             = _outstanding_jobs_sum.load();
    int32_t sum_ranks_not_completely_idle   = 0;
    int32_t sum_outstanding                 = 0;

    for(int j = 0; j < chameleon_comm_size; j++) {
        //DBP("action_handle_gather_request - values for rank %d: all_in_tw=%d, outstanding_jobs=%d, load=%d\n", j, _session_data.buffer_load_values[j*3], buffer_load_values[(j*3)+1], buffer_load_values[(j*3)+2]);
        int tmp_tw                              = _session_data.buffer_load_values[j*3];
        _outstanding_jobs_ranks[j]              = _session_data.buffer_load_values[(j*3)+1];
        _load_info_ranks[j]                     = _session_data.buffer_load_values[(j*3)+2];
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
    _session_data.request_gather_created = 0;
    
    #if OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED
    if(_session_data.last_known_sum_outstanding == -1) {
        _session_data.last_known_sum_outstanding = sum_outstanding;
        _session_data.offload_triggered = 0;
        // DBP("action_handle_gather_request - sum outstanding operations=%d, nr_open_requests_send=%d\n", *last_known_sum_outstanding, request_manager_send.getNumberOfOutstandingRequests());
    } else {
        // check whether changed.. only allow new offload after change
        if(_session_data.last_known_sum_outstanding != sum_outstanding) {
            _session_data.last_known_sum_outstanding = sum_outstanding;
            _session_data.offload_triggered = 0;
            // DBP("action_handle_gather_request - sum outstanding operations=%d, nr_open_requests_send=%d\n", *last_known_sum_outstanding, request_manager_send.getNumberOfOutstandingRequests());
        }
    }
    #else
    _session_data.offload_triggered = 0;
    #endif /* OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED */

    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_exchange_outstanding);
    #endif
}

inline void action_task_migration() {
    // only check for offloading if enough local tasks available and exchange has happend at least once
    #if FORCE_MIGRATION
    if(_comm_thread_load_exchange_happend && _session_data.offload_triggered.load() == 0) {
    #else
    // also only proceed if offloading not already performed.. wait for new load exchange
    if(_comm_thread_load_exchange_happend && _local_tasks.dup_size() + _replicated_local_tasks.dup_size() >= MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION && _session_data.offload_triggered.load() == 0) {
    #endif

        // Strategies for speculative load exchange
        // - If we find a rank with load = 0 ==> offload directly
        // - Should we look for the minimum? Might be a critical part of the program because several ranks might offload to that rank
        // - Just offload if there is a certain difference between min and max load to avoid unnecessary offloads
        // - Sorted approach: rank with highest load should offload to rank with minimal load
        // - Be careful about balance between computational complexity of calculating the offload target and performance gain that can be achieved
        
            #ifdef TRACE
            VT_BEGIN_CONSTRAINED(event_offload_decision);
            #endif

            int strategy_type;

            // reset values to zero
            std::fill(_session_data.tasks_to_offload.begin(), _session_data.tasks_to_offload.end(), 0);
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
                cham_t_status.cham_t_callback_select_num_tasks_to_offload(&(_session_data.tasks_to_offload[0]), &(_load_info_ranks[0]), num_tasks_local, num_tasks_stolen);
            } else {
                strategy_type = 0;
                num_tasks_local     = _local_tasks.dup_size();
                compute_num_tasks_to_offload(_session_data.tasks_to_offload, _load_info_ranks, num_tasks_local, num_tasks_stolen);
            }
            #else
            strategy_type = 0;
            num_tasks_local     = _local_tasks.dup_size();
            compute_num_tasks_to_offload(_session_data.tasks_to_offload, _load_info_ranks, num_tasks_local, num_tasks_stolen);
            #endif

            #if CHAM_STATS_RECORD
            _num_migration_decision_performed++;
            #endif

            #ifdef TRACE
            VT_END_W_CONSTRAINED(event_offload_decision);
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
                        }
                    }

                    std::vector<int> keys;
                    keys.reserve(map_task_num.size());

                    for(auto kv : map_task_num) {
                        keys.push_back(kv.first);
                    }

                    for (auto &r_id : keys) {
                        int num_tasks = map_task_num[r_id];
                        cham_migratable_task_t **cur_tasks = (cham_migratable_task_t**) malloc(num_tasks*sizeof(cham_migratable_task_t*));
                        for(int i=0; i <num_tasks; i++) {
                          cur_tasks[i]= map_task_vec[r_id][i];
                          cur_tasks[i]->is_migrated_task = 1;
                          #if CHAM_REPLICATION_MODE >= 3
                          cur_tasks[i]->is_replicated_task = 1;
                          cur_tasks[i]->replication_ranks.push_back(r_id);
                          #endif  /* CHAM_REPLICATION_MODE */
                        }

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
                for(int r=0; r<_session_data.tasks_to_offload.size(); r++) {
                    if(r != chameleon_comm_rank) {
                        // block until no active offload for rank any more
                        if(_active_migrations_per_target_rank[r].load() == 0) {
                            int num_tasks_to_migrate = _session_data.tasks_to_offload[r];
                            if(num_tasks_to_migrate)
                     	      //printf("action_task_migratino, active migrations for %d = %d, num_to_migrate = %d load rank 0: %d load rank 1: %d\n", r, _active_migrations_per_target_rank[r], num_tasks_to_migrate, _load_info_ranks[0], _load_info_ranks[1]);
                     	    //DBP("action_task_migratino, active migrations for %d = %d, num_to_migrate = %d\n", r , _active_migrations_per_target_rank[r], num_tasks_to_migrate);

			    #if CHAM_REPLICATION_MODE==4
                            DBP("action_task_migration - looking for active replication victim %d\n", r);
                            if(_active_replication_victims.find(r)!=_active_replication_victims.end()) {
                        	    int tasks_to_activate =  std::min( (int) MAX_TASKS_PER_RANK_TO_ACTIVATE_AT_ONCE.load(),std::min(_num_replicated_local_tasks_per_victim[r], num_tasks_to_migrate));  // std::min(_num_replicated_local_tasks_per_victim[r], num_tasks_to_migrate);
            //            	    tasks_to_activate = 0;
                                    //int tasks_to_activate =  std::min( (int) MAX_TASKS_PER_RANK_TO_ACTIVATE_AT_ONCE.load(),std::min(_num_replicated_local_tasks_per_victim[r], num_tasks_to_migrate));  // std::min(_num_replicated_local_tasks_per_victim[r], num_tasks_to_migrate);
                        	    if(tasks_to_activate>0) {
                     	        //       printf("action_task_migratino, active migrations for %d = %d, num_to_migrate = %d, num_tasks_to_activate = %d load rank 0: %d load rank 1: %d\n", r , _active_migrations_per_target_rank[r], num_tasks_to_migrate, tasks_to_activate, _load_info_ranks[0], _load_info_ranks[1]);
                        	       DBP("action_task_migration - activating %d replicated tasks on rank %d\n", tasks_to_activate, r);
                        	       activate_replicated_tasks_on_rank(tasks_to_activate, r);
                            	       _num_replicated_local_tasks_per_victim[r]-= tasks_to_activate;
                            	       #if (CHAM_STATS_RECORD)  
                                       _num_tasks_activated += tasks_to_activate;
				       #endif
                                       _session_data.offload_triggered = 1;
                                       num_tasks_to_migrate -= tasks_to_activate; //continue with task migration if it makes sense
                                       num_tasks_to_migrate = std::max(0, num_tasks_to_migrate);
                                     //  break;
                        	    }
                            }
                            //continue;
			    #endif
                     	    //        printf("action_task_migratino, active migrations for %d = %d, num_to_migrate = %d\n", r , _active_migrations_per_target_rank[r], num_tasks_to_migrate);
                     	    //if(num_tasks_to_migrate)
                                  //     printf("action_task_migration, rank %d , num_to_migrate = %d, load rank 0 = %d, load rank 1 =%d\n", r, num_tasks_to_migrate, _load_info_ranks[0], _load_info_ranks[1]);

                            if(num_tasks_to_migrate==0) continue;
 
                            int num_tasks = 0;
                            cham_migratable_task_t **cur_tasks = (cham_migratable_task_t**) malloc(sizeof(cham_migratable_task_t*)*num_tasks_to_migrate);

                            for(int t=0; t < num_tasks_to_migrate; t++) {
                                cham_migratable_task_t *task = _local_tasks.pop_front();
                                if(task) {
                                    cur_tasks[num_tasks] = task;
                                    cur_tasks[num_tasks]->is_migrated_task = 1;
                                    #if CHAM_REPLICATION_MODE >= 3
                                    cur_tasks[num_tasks]->is_replicated_task = 1;
                                    cur_tasks[num_tasks]->replication_ranks.push_back(r);
                                    #endif  /* CHAM_REPLICATION_MODE */                             
                                    num_tasks++;
                             
                                    // stop when limit reached
                                    if(num_tasks >= MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE) {
                                        // RELP("num_tasks:%d, MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE:%f\n", num_tasks, MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE.load())
                                        break;
                                    }
                                }
                            }

                            if(num_tasks > 0) {
                                // double victim_load  = (double) _load_info_ranks[r];
                                // double cur_diff     = (my_current_load-victim_load);
                                // double cur_ratio    = cur_diff / victim_load;

                                #if OFFLOAD_SEND_TASKS_SEPARATELY
                                // RELP("Migrating\t%d\ttasks (separately one by one) to rank:\t%d\tload:\t%f\tload_victim:\t%f\tratio:\t%f\tdiff:\t%f\n", num_tasks, r, my_current_load, victim_load, cur_ratio, cur_diff);
                                for(int i_task = 0; i_task < num_tasks; i_task++) {
                                    offload_tasks_to_rank(&cur_tasks[i_task], 1, r);
                                }
                                #else
                                // RELP("Migrating\t%d\ttasks to rank:\t%d\tload:\t%f\tload_victim:\t%f\tratio:\t%f\tdiff:\t%f\n", num_tasks, r, my_current_load, victim_load, cur_ratio, cur_diff);
                                offload_tasks_to_rank(&cur_tasks[0], num_tasks, r);
                                #endif

                                offload_done = true;
                            }
                            else {
                                free(cur_tasks);
                            }
                        }
                    }
                }
            }
            if(offload_done) {
                _session_data.offload_triggered = 1;
                #if CHAM_STATS_RECORD
                _num_migration_done++;
                #endif /* CHAM_STATS_RECORD */    
            }
    }
}

inline void action_send_back_stolen_tasks(cham_migratable_task_t *cur_task, RequestManager *request_manager_send) {
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(event_send_back);
    #endif

    DBP("send_back_stolen_tasks - sending back data to rank %d with tag %d for (task_id=%ld)\n", cur_task->source_mpi_rank, cur_task->task_id, cur_task->task_id);
    
    int num_bytes_sent = 0;
    double start_time_requests = 0;

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
            print_arg_info("send_back_stolen_tasks", cur_task, i);
            memcpy(cur_ptr, cur_task->arg_hst_pointers[i], cur_task->arg_sizes[i]);
            cur_ptr += cur_task->arg_sizes[i];
        }
    }
    MPI_Request request;
    #if CHAM_STATS_RECORD
    num_bytes_sent += tmp_size_buff;
    _stats_bytes_send_per_message.add_stat_value((double)tmp_size_buff);
    start_time_requests = omp_get_wtime();
    #endif 
    if(cur_task->is_replicated_task)
      MPI_Issend(buff, tmp_size_buff, MPI_BYTE, cur_task->source_mpi_rank, cur_task->task_id, chameleon_comm_mapped, &request);
    else
      MPI_Isend(buff, tmp_size_buff, MPI_BYTE, cur_task->source_mpi_rank, cur_task->task_id, chameleon_comm_mapped, &request);

    cham_migratable_task_t **tasks = (cham_migratable_task_t **) malloc(sizeof(cham_migratable_task_t *));
    tasks[0] = cur_task;
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = send_back_handler;
    request_manager_send->submitRequests(start_time_requests, 
        cur_task->task_id, 
        cur_task->source_mpi_rank, 
        1, 
        &request, 
        tmp_size_buff, 
        0, 
        cur_handler, 
        sendBack, 
        buff, 
        tasks, 
        1);
    
    #elif OFFLOAD_DATA_PACKING_TYPE > 0
    #if CHAM_STATS_RECORD
    start_time_requests = omp_get_wtime();
    #endif
    #if OFFLOAD_DATA_PACKING_TYPE == 1
    MPI_Request *requests = new MPI_Request[cur_task->arg_num];
    int num_requests = 0;
    for(int i = 0; i < cur_task->arg_num; i++) {
        if(cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
            #if CHAM_STATS_RECORD
            num_bytes_sent += cur_task->arg_sizes[i];
            _stats_bytes_send_per_message.add_stat_value((double)cur_task->arg_sizes[i]);
            #endif
            //DBP("action_send_back_stolen_tasks - argument %i, size %i\n", i, cur_task->arg_sizes[i]);
            if(cur_task->is_replicated_task)
              MPI_Issend(cur_task->arg_hst_pointers[i], cur_task->arg_sizes[i], MPI_BYTE, cur_task->source_mpi_rank, cur_task->task_id, chameleon_comm_mapped, &requests[num_requests++]);
            else
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
        if(cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
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
    #if CHAM_STATS_RECORD
    int size = 0;
    MPI_Type_size(type_mapped_vars, &size);
    num_bytes_sent += size;
    _stats_bytes_send_per_message.add_stat_value((double)size);
    start_time_requests = omp_get_wtime();
    #endif
    if(cur_task->is_replicated_task)
      ierr = MPI_Issend(MPI_BOTTOM, 1, type_mapped_vars, cur_task->source_mpi_rank, cur_task->task_id, chameleon_comm_mapped, &requests[0]);
    else
      ierr = MPI_Isend(MPI_BOTTOM, 1, type_mapped_vars, cur_task->source_mpi_rank, cur_task->task_id, chameleon_comm_mapped, &requests[0]);
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_free(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);
    #endif

    cham_migratable_task_t **tasks = (cham_migratable_task_t **) malloc(sizeof(cham_migratable_task_t *));
    tasks[0] = cur_task;
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = send_back_handler;
    request_manager_send->submitRequests(start_time_requests, 
        cur_task->task_id, 
        cur_task->source_mpi_rank, 
        num_requests, 
        &requests[0], 
        num_bytes_sent, 
        0, 
        cur_handler, 
        sendBack, 
        nullptr, 
        tasks, 
        1);
    delete[] requests;
    #endif /* OFFLOAD_DATA_PACKING_TYPE */

    //_num_remote_tasks_outstanding--;
    //DBP("send_back_stolen_tasks - decrement stolen outstanding count for task %ld new count: %ld\n", cur_task->task_id, _num_remote_tasks_outstanding.load());

    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_send_back);
    #endif
}

inline void action_handle_cancel_request(MPI_Status *cur_status_cancel) {

    TYPE_TASK_ID task_id = -1;
    MPI_Recv(&task_id, 1, MPI_INTEGER, cur_status_cancel->MPI_SOURCE, 0, chameleon_comm_cancel, MPI_STATUS_IGNORE);
    DBP("action_handle_cancel_request - received cancel request for task id %d\n", task_id);

    cham_migratable_task_t *res = _replicated_remote_tasks.pop_task_by_id(task_id);

    bool is_active = true;
#if CHAM_REPLICATION_MODE==4
    is_active = false;
#endif

    if(!res) {
      res = _stolen_remote_tasks.pop_task_by_id(task_id);
      is_active = true;
    }

    if(!res) {
      res = _replicated_migrated_tasks.pop_task_by_id(task_id);
      is_active = true;
    }

    if(res) {
      DBP("action_handle_cancel_request - cancelling task with task_id %ld\n", task_id);
      _map_tag_to_remote_task.find_and_erase(task_id);

        // we haven't processed this task yet so we can safely delete this task
        _map_overall_tasks.erase(task_id);

#if CHAM_REPLICATION_MODE!=4
        _num_replicated_remote_tasks_outstanding--; assert(_num_replicated_remote_tasks_outstanding>=0);

        if(res->is_migrated_task)
            _num_replicated_and_migrated_remote_tasks_outstanding--;
#endif
        // decrement load counter and ignore send back
        if(is_active)
          _num_remote_tasks_outstanding--; assert(_num_remote_tasks_outstanding>=0);
        DBP("receive_remote_tasks(cancel) - decrement stolen outstanding count for task %ld new %d\n", task_id, _num_remote_tasks_outstanding.load());

        free_migratable_task(res, 1);
        #if CHAM_STATS_RECORD
        _num_tasks_canceled++;
        #endif
    }
    else {
        DBP("action_handle_cancel_request - received cancel request for task_id %ld but could not find task\n", task_id);
        _cancelled_task_ids.insert(task_id);
    }
}

inline void action_handle_activate_request(MPI_Status *cur_status_activate) {
	int ntasks = 0;
	MPI_Recv(&ntasks, 1, MPI_INTEGER, cur_status_activate->MPI_SOURCE, 0, chameleon_comm_activate, MPI_STATUS_IGNORE);
	DBP("action_handle_activate_request - I should activate %d tasks from rank %d\n", ntasks, cur_status_activate->MPI_SOURCE);
	activate_remote_replicated_tasks(cur_status_activate->MPI_SOURCE, ntasks);
}

inline void action_post_recvback_requests(cham_migratable_task_t *task_entry, int mpi_source, int mpi_tag, RequestManager *request_manager_receive) {
    DBP("action_post_recvback_requests - posting receive requests for task with id %ld\n", task_entry->task_id);
    //remove from replicated task queue -> corresponds to local task cancellation
    //if(task_entry->is_replicated_task)
    //  _replicated_local_tasks.remove(task_entry);

    //we can safely receive back as usual
    
    double start_time_requests, cur_time;
    int num_bytes_received = 0;
    int ierr;
    
    #if OFFLOAD_DATA_PACKING_TYPE == 0
    //entry is removed in receiveBackHandler!
    // receive data back
    int recv_buff_size = task_entry->buffer_size_output_data;
    // MPI_Get_count(cur_status_receiveBack, MPI_BYTE, &recv_buff_size);
    void * buffer = malloc(recv_buff_size);
    
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(event_recv_back);
    #endif

    #if CHAM_STATS_RECORD
    start_time_requests = cur_time = omp_get_wtime();
    #endif

    #if MPI_BLOCKING
    ierr = MPI_Recv(buffer, recv_buff_size, MPI_BYTE, mpi_source, mpi_tag, chameleon_comm_mapped, MPI_STATUS_IGNORE); assert(ierr==MPI_SUCCESS);
    #else
    MPI_Request request;
    ierr = MPI_Irecv(buffer, recv_buff_size, MPI_BYTE, mpi_source, mpi_tag, chameleon_comm_mapped, &request); assert(ierr==MPI_SUCCESS);
    #endif
    #if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    num_bytes_received += recv_buff_size;
    _stats_bytes_recv_per_message.add_stat_value((double)recv_buff_size);
    #if MPI_BLOCKING
    add_throughput_recv(cur_time, num_bytes_received);
    #endif
    #endif

    #if MPI_BLOCKING
    receive_back_handler(buffer, mpi_tag, mpi_source, nullptr, 0);
    #else
    cham_migratable_task_t** p_tasks = (cham_migratable_task_t**) malloc(sizeof(cham_migratable_task_t*));
    p_tasks[0] = task_entry;
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = receive_back_handler;
    request_manager_receive->submitRequests( start_time_requests, mpi_tag, mpi_source, 1, 
                                            &request,
                                            recv_buff_size,
                                            0,
                                            cur_handler,
                                            recvBack,
                                            buffer,
                                            p_tasks,
                                            1);
    #endif

    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_recv_back);
    #endif

    #elif OFFLOAD_DATA_PACKING_TYPE == 1

    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(event_recv_back);
    #endif

    MPI_Request *requests = new MPI_Request[task_entry->arg_num];  
    int j = 0;
    #if CHAM_STATS_RECORD
    start_time_requests = cur_time = omp_get_wtime();
    #endif
    for(int i = 0; i < task_entry->arg_num; i++) {
        if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
            #if MPI_BLOCKING
            ierr = MPI_Recv(task_entry->arg_hst_pointers[i], task_entry->arg_sizes[i], MPI_BYTE, mpi_source, mpi_tag,
                                                                        chameleon_comm_mapped, MPI_STATUS_IGNORE); assert(ierr==MPI_SUCCESS);
            #else
            ierr = MPI_Irecv(task_entry->arg_hst_pointers[i], task_entry->arg_sizes[i], MPI_BYTE, mpi_source, mpi_tag,
                                                                        chameleon_comm_mapped, &requests[j++]); assert(ierr==MPI_SUCCESS);
            #endif                               
            #if CHAM_STATS_RECORD
            num_bytes_received += task_entry->arg_sizes[i];
            _stats_bytes_recv_per_message.add_stat_value((double)task_entry->arg_sizes[i]);
            #endif
        }
    }

    #if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    #if MPI_BLOCKING
    add_throughput_recv(cur_time, num_bytes_received);
    #endif
    #endif
    
    #if MPI_BLOCKING
    receive_back_handler(nullptr, mpi_tag, mpi_source, nullptr, 0);
    #else
    cham_migratable_task_t** p_tasks = (cham_migratable_task_t**) malloc(sizeof(cham_migratable_task_t*));
    p_tasks[0] = task_entry;
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = receive_back_handler;
    request_manager_receive->submitRequests( start_time_requests, mpi_tag, mpi_source, j, 
                                        &requests[0],
                                        num_bytes_received,
                                        0,
                                        cur_handler,
                                        recvBack,
                                        nullptr,
                                        p_tasks,
                                        1);
    #endif
    delete[] requests;
    
    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_recv_back);
    #endif

    #elif OFFLOAD_DATA_PACKING_TYPE == 2
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(event_recv_back);
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
    int j = 0;
    for(int i=0; i < task_entry->arg_num; i++) {
        if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
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
    #if CHAM_STATS_RECORD
    int size = 0;
    MPI_Type_size(type_mapped_vars, &size);
    num_bytes_received += size;
    _stats_bytes_recv_per_message.add_stat_value((double)size);
    start_time_requests = cur_time = omp_get_wtime();
    #endif
    #if MPI_BLOCKING
    ierr = MPI_Recv(MPI_BOTTOM, 1, type_mapped_vars, mpi_source, mpi_tag, chameleon_comm_mapped, MPI_STATUS_IGNORE);
    #else
    ierr = MPI_Irecv(MPI_BOTTOM, 1, type_mapped_vars, mpi_source, mpi_tag, chameleon_comm_mapped, &requests[0]);
    #endif
    assert(ierr==MPI_SUCCESS);
    ierr = MPI_Type_free(&type_mapped_vars);
    assert(ierr==MPI_SUCCESS);

    #if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    #if MPI_BLOCKING
    add_throughput_recv(cur_time, num_bytes_received);
    #endif
    #endif

    #if MPI_BLOCKING
    receive_back_handler(nullptr, mpi_tag, mpi_source, nullptr, 0);
    #else
    cham_migratable_task_t** p_tasks = (cham_migratable_task_t**) malloc(sizeof(cham_migratable_task_t*));
    p_tasks[0] = task_entry;
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = receive_back_handler;
    request_manager_receive->submitRequests( start_time_requests, mpi_tag, mpi_source, 1, 
                                        &requests[0],
                                        num_bytes_received,
                                        0,
                                        cur_handler,
                                        recvBack,
                                        nullptr,
                                        p_tasks,
                                        1);
    #endif
    delete[] requests;

    #ifdef TRACE 
    VT_END_W_CONSTRAINED(event_recv_back);
    #endif
    #endif /* OFFLOAD_DATA_PACKING_TYPE */
}

inline void action_handle_recvback_request(MPI_Status *cur_status_receiveBack, RequestManager *request_manager_receive) {
	DBP("action_handle_recvback_request - looking for task for tag %ld\n", cur_status_receiveBack->MPI_TAG);
	cham_migratable_task_t *task_entry = _map_offloaded_tasks_with_outputs.find(cur_status_receiveBack->MPI_TAG);
	assert(task_entry!=nullptr);
	DBP("action_handle_recvback_request - found task %p\n", task_entry);
    if(task_entry) {
        #if CHAM_STATS_RECORD
        double cur_time;
        #endif
        int num_bytes_received = 0;
        int ierr;

        DBP("action_handle_recvback_request - receiving back task with id %ld\n", task_entry->task_id);
        task_entry->num_outstanding_recvbacks--;
        DBP("action_handle_recvback_request - decrement outstanding recvbacks for task with id %ld new count: %d\n", task_entry->task_id, task_entry->num_outstanding_recvbacks);
        assert(task_entry->num_outstanding_recvbacks>=0);

        // check if we still need to receive the task data back or replicated task is executed locally already
        //bool expected = false;
        //bool desired = true;
        // DBP("action_handle_recvback_request - performing CAS for task with id %ld, flag %d\n", task_entry->task_id, task_entry->sync_commthread_lock.load());
        //assert(task_entry->sync_commthread_lock.load()==false);
        //bool exchanged = task_entry->result_in_progress.compare_exchange_strong(expected, desired);
        bool post_receive = true;
        if(task_entry->is_replicated_task) {
          //flush sending queue as otherwise race may occur where task has been sent, but is not in replicated_local_task queue
          request_manager_send.progressRequests(0);
          cham_migratable_task_t *task_from_queue = _replicated_local_tasks.pop_task_by_id(task_entry->task_id);
          if(task_from_queue==nullptr) post_receive=false;
        }
        //assert(exchanged);
        // DBP("action_handle_recvback_request - CAS: expected = %d, desired = %d, exchanged = %d\n", expected, desired, exchanged);
        //atomic CAS   
        double start_time_requests = 0;
        if(post_receive) {
            action_post_recvback_requests(task_entry, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG, request_manager_receive);
        }  //CAS
        else // CAS didn't succeed -> we need to receive data into trash buffer
        {
            DBP("Late receive back occured for replicated task, task_id %ld\n", task_entry->task_id);
            int msg_size = 0;
            void *trash_buffer;

            #if OFFLOAD_DATA_PACKING_TYPE == 0
            MPI_Get_count(cur_status_receiveBack, MPI_BYTE, &msg_size);
            trash_buffer = malloc(msg_size);
            MPI_Request request;

            #if CHAM_STATS_RECORD
            start_time_requests = cur_time = omp_get_wtime();
            #endif

            #if MPI_BLOCKING
            ierr = MPI_Recv(trash_buffer, msg_size, MPI_BYTE, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG, chameleon_comm_mapped, MPI_STATUS_IGNORE);
            #else
            ierr = MPI_Irecv(trash_buffer, msg_size, MPI_BYTE, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG, chameleon_comm_mapped, &request); assert(ierr==MPI_SUCCESS);
            #endif

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            //TODO: count trash receives!
            num_bytes_received += msg_size;
            _stats_bytes_recv_per_message.add_stat_value((double)msg_size);
            #if MPI_BLOCKING
            add_throughput_recv(cur_time, num_bytes_received);
            #endif
            #endif

            #if MPI_BLOCKING
            receive_back_handler(trash_buffer, cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, nullptr, 0);
            #else
            cham_migratable_task_t** p_tasks = (cham_migratable_task_t**) malloc(sizeof(cham_migratable_task_t*));
            p_tasks[0] = task_entry;
            std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = receive_back_trash_handler;
            request_manager_receive->submitRequests( start_time_requests, cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, 1, 
                                                    &request,
                                                    num_bytes_received,
                                                    0,
                                                    cur_handler,
                                                    recvBackTrash,
                                                    trash_buffer,
				   		    p_tasks,
						    1);
            #endif

            #elif OFFLOAD_DATA_PACKING_TYPE > 0 // TODO: need to take care of Type 2
            int num_requests;
            MPI_Request *requests;

            #if OFFLOAD_DATA_PACKING_TYPE == 1
            num_requests = task_entry->arg_num;
            requests = new MPI_Request[num_requests];

            for(int i=0; i< task_entry->arg_num; i++) {
                if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                    msg_size += std::max<int64_t>(1,task_entry->arg_sizes[i]);
                }
            }
            DBP("action_handle_recvback_request - allocating trash buffer of size %ld bytes\n", msg_size);
            trash_buffer = malloc(msg_size);
            // current position for trash data
            char *cur_ptr_pos = (char*)trash_buffer;

            int j = 0;
            #if CHAM_STATS_RECORD
            start_time_requests = cur_time = omp_get_wtime();
            #endif
            for(int i = 0; i < task_entry->arg_num; i++) {
                if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                    #if MPI_BLOCKING
                    ierr = MPI_Recv(cur_ptr_pos, task_entry->arg_sizes[i], MPI_BYTE, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG,
                                                                                chameleon_comm_mapped, MPI_STATUS_IGNORE); assert(ierr==MPI_SUCCESS);
                    #else
                    ierr = MPI_Irecv(cur_ptr_pos, task_entry->arg_sizes[i], MPI_BYTE, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG,
                                                                                chameleon_comm_mapped, &requests[j++]); assert(ierr==MPI_SUCCESS);
                    DBP("action_handle_recvback_request - posting irecv to %p of size %ld bytes\n", cur_ptr_pos, task_entry->arg_sizes[i]);
                    #endif
                    cur_ptr_pos += task_entry->arg_sizes[i];
                    #if CHAM_STATS_RECORD
                    num_bytes_received += task_entry->arg_sizes[i];
                    _stats_bytes_recv_per_message.add_stat_value((double)task_entry->arg_sizes[i]);
                    #endif
                }
            }
            num_requests = j;

            #elif OFFLOAD_DATA_PACKING_TYPE == 2
            num_requests = 1;
            requests = new MPI_Request[num_requests];

            MPI_Datatype type_mapped_vars;
            int num_outputs = 0;
            for(int i=0; i< task_entry->arg_num; i++) {
                if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                    msg_size += task_entry->arg_sizes[i];
                    num_outputs++;
                }
            }
            trash_buffer = malloc(msg_size);            
            // current position for trash data
            char *cur_ptr_pos = (char*)trash_buffer;

            MPI_Datatype separate_types[num_outputs];
            int blocklen[num_outputs];
            MPI_Aint disp[num_outputs];
            int j = 0;
            for(int i=0; i < task_entry->arg_num; i++) {
                if(task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                    separate_types[j]   = MPI_BYTE;
                    blocklen[j]         = task_entry->arg_sizes[i];
                    ierr = MPI_Get_address(cur_ptr_pos, &(disp[j]));
                    assert(ierr==MPI_SUCCESS);
                    cur_ptr_pos += task_entry->arg_sizes[i];
                    j++;
                }
            }
            ierr = MPI_Type_create_struct(num_outputs, blocklen, disp, separate_types, &type_mapped_vars);
            assert(ierr==MPI_SUCCESS);
            ierr = MPI_Type_commit(&type_mapped_vars);
            assert(ierr==MPI_SUCCESS);

            #if CHAM_STATS_RECORD
            int size = 0;
            MPI_Type_size(type_mapped_vars, &size);
            num_bytes_received += size;
            _stats_bytes_recv_per_message.add_stat_value((double)size);
            start_time_requests = cur_time = omp_get_wtime();
            #endif
            #if MPI_BLOCKING
            ierr = MPI_Recv(MPI_BOTTOM, 1, type_mapped_vars, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG, chameleon_comm_mapped, MPI_STATUS_IGNORE);
            #else
            ierr = MPI_Irecv(MPI_BOTTOM, 1, type_mapped_vars, cur_status_receiveBack->MPI_SOURCE, cur_status_receiveBack->MPI_TAG, chameleon_comm_mapped, &requests[0]);
            #endif
            assert(ierr==MPI_SUCCESS);
            ierr = MPI_Type_free(&type_mapped_vars);
            assert(ierr==MPI_SUCCESS);
            #endif

            #if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            #if MPI_BLOCKING
            add_throughput_recv(cur_time, num_bytes_received);
            #endif
            #endif
            
            #if MPI_BLOCKING
            receive_back_handler(trash_buffer, cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, nullptr, 0);
            #else
            cham_migratable_task_t** p_tasks = (cham_migratable_task_t**) malloc(sizeof(cham_migratable_task_t*));
            p_tasks[0] = task_entry;
            std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = receive_back_trash_handler;
            request_manager_receive->submitRequests( start_time_requests, cur_status_receiveBack->MPI_TAG, cur_status_receiveBack->MPI_SOURCE, num_requests, 
                                                &requests[0],
                                                num_bytes_received,
                                                0,
                                                cur_handler,
                                                recvBackTrash,
                                                trash_buffer,
						p_tasks,
						1);
            #endif
            delete[] requests;
            #endif /* OFFLOAD_DATA_PACKING_TYPE */
        }
    } //match
}

inline void action_handle_recv_request(MPI_Status *cur_status_receive, RequestManager *request_manager_receive) {
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(event_receive_tasks);
    #endif

    DBP("Incoming receive request for task id %d from rank %d\n", cur_status_receive->MPI_TAG, cur_status_receive->MPI_SOURCE);
    
    int recv_buff_size = 0;
    MPI_Get_count(cur_status_receive, MPI_BYTE, &recv_buff_size);
    void *buffer = malloc(recv_buff_size);

    MPI_Request request = MPI_REQUEST_NULL;
    double start_time_requests = 0;

    #if CHAM_STATS_RECORD
    double cur_time;
    start_time_requests = cur_time = omp_get_wtime();
    #endif
    int num_bytes_received = 0;

    #if MPI_BLOCKING
    int res = MPI_Recv(buffer, recv_buff_size, MPI_BYTE, cur_status_receive->MPI_SOURCE, cur_status_receive->MPI_TAG, chameleon_comm, MPI_STATUS_IGNORE);
    #else
    int res = MPI_Irecv(buffer, recv_buff_size, MPI_BYTE, cur_status_receive->MPI_SOURCE, cur_status_receive->MPI_TAG, chameleon_comm, &request);
    #endif
    assert(res==MPI_SUCCESS);

    #if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    num_bytes_received += recv_buff_size;
    #if OFFLOAD_DATA_PACKING_TYPE == 0 // disable tracking for very short meta data messages because it might destroy reliability of statistics
    _stats_bytes_recv_per_message.add_stat_value((double)recv_buff_size);
    #endif
    #if MPI_BLOCKING
    add_throughput_recv(cur_time, recv_buff_size);
    #endif
    #endif

    #if MPI_BLOCKING
    receive_handler(buffer, cur_status_receive->MPI_TAG, cur_status_receive->MPI_SOURCE, nullptr, 0);
    #else
    std::function<void(void*, int, int, cham_migratable_task_t**, int)> cur_handler = receive_handler;
    request_manager_receive->submitRequests( start_time_requests, cur_status_receive->MPI_TAG, 
                                    cur_status_receive->MPI_SOURCE,
                                    1, 
                                    &request,
                                    num_bytes_received,
                                    0,
                                    cur_handler,
                                    recv,
                                    buffer);
    #endif
    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_receive_tasks);
    #endif
}

inline bool action_task_replication() {
    //if (chameleon_comm_rank == 1) return false;

    static cham_t_replication_info_t* replication_infos = nullptr;
    if(_comm_thread_load_exchange_happend && _local_tasks.dup_size() >= MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION ) {
    	DBP("action_task_replication - selecting tasks to replicate\n");
        //clean up replication infos from previous distributed taskwait
    	while(!_replication_infos_list.empty()) {
    		free_replication_info(_replication_infos_list.pop_front());
    	}

        if(!replication_infos) free(replication_infos);

    	int num_tasks_local = _local_tasks.dup_size();

        int num_rep_infos = 0;
        #if CHAMELEON_TOOL_SUPPORT && !FORCE_MIGRATION
        if(cham_t_status.enabled && cham_t_status.cham_t_callback_select_num_tasks_to_replicate) {
               replication_infos = cham_t_status.cham_t_callback_select_num_tasks_to_replicate(
            		             &(_load_info_ranks[0]), num_tasks_local, &num_rep_infos);

        } else {
        	   replication_infos = compute_num_tasks_to_replicate( _load_info_ranks, num_tasks_local, &num_rep_infos);
        }
        #else
          replication_infos = compute_num_tasks_to_replicate( _load_info_ranks, num_tasks_local, &num_rep_infos);
        #endif

        for( int r=0; r<num_rep_infos; r++) {
           _replication_infos_list.push_back(&replication_infos[r]);
        }
        return true;
    }
    return false;
}

inline void action_task_replication_send() {   

  if(_comm_thread_load_exchange_happend && _local_tasks.dup_size() + _replicated_local_tasks.dup_size() >= MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION && _session_data.offload_triggered.load() == 0) {

    cham_t_replication_info_t *rep_info;
    if(!_replication_infos_list.empty())
      rep_info = _replication_infos_list.pop_front();
    else
      return;
      
    if(rep_info->num_tasks && _active_migrations_per_target_rank[rep_info->replication_ranks[0]]>0) {
    	_replication_infos_list.push_back(rep_info);
    	return;
    }
   

    //if(request_manager_receive.getNumberOfOutstandingRequests()>0) 
    //    return;

    //Todo: don't replicate if any target has active migrations
    if(rep_info->num_tasks<=0) return;


    DBP("action_task_replication_send - trying to transfer replicated task, size: %d\n", _replication_infos_list.size());
    //printf("action_task_replication_send - trying to transfer replicated task to rank %d, size: %d\n", rep_info->replication_ranks[0], _replication_infos_list.size());

    cham_migratable_task_t *cur_task = _local_tasks.pop_front();

    if(cur_task) {
      cham_migratable_task_t **cur_task_list = (cham_migratable_task_t **) malloc(sizeof(cham_migratable_task_t*));
      cur_task_list[0] = cur_task;
      cur_task->is_replicated_task = 1;
      cur_task->is_migrated_task = 0;
      for(int i=0; i<rep_info->num_replication_ranks; i++) {
        cur_task->replication_ranks.push_back(rep_info->replication_ranks[i]);
        add_active_replication_victim(rep_info->replication_ranks[i], 1);
        offload_tasks_to_rank(cur_task_list, 1, rep_info->replication_ranks[i], true);
      }
      rep_info->num_tasks--;
    }

    //round-robin through replication infos if we still need to replicate tasks
    if(rep_info->num_tasks)
    	_replication_infos_list.push_back(rep_info);
 
  }
}

void action_communication_progression(int comm_thread) {
    #if ENABLE_TASK_MIGRATION || CHAM_REPLICATION_MODE>0
    #if CHAM_REPLICATION_MODE>=2
    _mtx_cancellation.lock();
    request_manager_cancel.progressRequests(comm_thread);
    _mtx_cancellation.unlock();
    #endif
    #if CHAM_REPLICATION_MODE==4
    // Todo: progress requests for activation of replicated tasks
    #endif
    #ifdef TRACE
    VT_BEGIN_CONSTRAINED(event_progress_send);
    #endif
    request_manager_send.progressRequests(comm_thread);
    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_progress_send);
    VT_BEGIN_CONSTRAINED(event_progress_recv);
    #endif
    request_manager_receive.progressRequests(comm_thread);
    #ifdef TRACE
    VT_END_W_CONSTRAINED(event_progress_recv);
    #endif
    #endif /* ENABLE_TASK_MIGRATION */

    // ==============================
    // ========== SEND / EXCHANGE
    // ==============================

    int request_gather_avail = 0;
    // avoid overwriting request and keep it up to date
    #if COMMUNICATION_MODE == 2
    if (_mtx_comm_progression.try_lock()) {
    #elif COMMUNICATION_MODE != 1
    if(comm_thread) {  // only execute that code if called from communication thread
    #endif /* COMMUNICATION_MODE */

    if(!_session_data.request_gather_created) {
        #ifdef TRACE
        VT_BEGIN_CONSTRAINED(event_create_gather_request);
        #endif
        action_create_gather_request();
        #ifdef TRACE
        VT_END_W_CONSTRAINED(event_create_gather_request);
        #endif
        _session_data.request_gather_created = 1;
        #if CHAM_STATS_RECORD
        _session_data.time_gather_posted = omp_get_wtime();
        #endif /* CHAM_STATS_RECORD */
    }

    #if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
    double cur_time = omp_get_wtime();
    #endif
    MPI_Test(&_session_data.request_gather_out, &request_gather_avail, &_session_data.status_gather_out);
    #if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
    cur_time = omp_get_wtime()-cur_time;
    if(cur_time>CHAM_SLOW_COMMUNICATION_THRESHOLD)
        _num_slow_communication_operations++;
    #endif

    if(request_gather_avail) {
        #if CHAM_STATS_RECORD
        _num_load_exchanges_performed++;

        double cur_diff = omp_get_wtime()-_session_data.time_gather_posted;
        atomic_add_dbl(_time_between_allgather_and_exchange_sum, cur_diff);
        _time_between_allgather_and_exchange_count++;

        // calculate time between two load exchanges discarding sleep times
        if(_comm_thread_load_exchange_happend) {
            cur_diff = omp_get_wtime()-_session_data.time_last_load_exchange;
            atomic_add_dbl(_time_between_load_exchange_sum, cur_diff);
            _time_between_load_exchange_count++;
        }
        #endif /* CHAM_STATS_RECORD */

        action_handle_gather_request();

        // set flag that exchange has happend
        if(!_comm_thread_load_exchange_happend) {
            _comm_thread_load_exchange_happend = 1;
        }

        #if CHAM_STATS_RECORD
        // save last time load exchange happend for current sync cycle
        _session_data.time_last_load_exchange = omp_get_wtime();
        #endif /* CHAM_STATS_RECORD */

        // Handle exit condition here to avoid that iallgather is posted after iteration finished
        bool exit_true = exit_condition_met(0,1);
        if(exit_true){
            _flag_comm_thread_sleeping              = 1;
            _comm_thread_service_stopped            = 1;
            _session_data.flag_thread_sleeping_set  = 1;
            #if CHAM_STATS_RECORD
            double time_commthread_elapsed = omp_get_wtime()-_session_data.time_start_comm;
            atomic_add_dbl(_time_commthread_active_sum, time_commthread_elapsed);
            _time_commthread_active_count++;
            assert(_num_active_communications_overall.load()==0);
            #endif /* CHAM_STATS_RECORD */
            assert(_remote_tasks_send_back.empty());
            assert(request_manager_cancel.getNumberOfOutstandingRequests()==0);

            // run routine to cleanup all things for current work phase
            cleanup_work_phase();

            DBP("action_communication_progression - thread went to sleep again due to exit condition\n");
            #if COMMUNICATION_MODE == 2
            _mtx_comm_progression.unlock();
            #endif
            return;
        }
        // else if(!request_gather_created) {
        //     // post Iallgather asap!
        //     #ifdef TRACE
        //     VT_BEGIN_CONSTRAINED(event_create_gather_request);
        //     #endif
        //     action_create_gather_request(&num_threads_in_tw, &(transported_load_values[0]), buffer_load_values, &request_gather_out);
        //     #ifdef TRACE
        //     VT_END_W_CONSTRAINED(event_create_gather_request);
        //     #endif
        //     request_gather_created = 1;
        //     #if CHAM_STATS_RECORD
        //     time_gather_posted = omp_get_wtime();
        //     #endif /* CHAM_STATS_RECORD */
        // }
    }

    #if CHAM_REPLICATION_MODE>0
    if(!_session_data.has_replicated && _session_data.num_threads_in_tw == _num_threads_active_in_taskwait) {
        _session_data.has_replicated = action_task_replication();
    }
    #endif

    #if ENABLE_TASK_MIGRATION
    action_task_migration();
    #endif /* ENABLE_TASK_MIGRATION */

    #if CHAM_REPLICATION_MODE>0
    if(!_session_data.has_replicated && _session_data.num_threads_in_tw == _num_threads_active_in_taskwait) {
        _session_data.has_replicated = action_task_replication();
    }
    #endif

    #if CHAM_REPLICATION_MODE>0
    if(_session_data.has_replicated && !_session_data.offload_triggered && !_session_data.is_migration_victim )
        action_task_replication_send();
    #endif

    // transfer back data of stolen tasks
    for (int i_sb = 0; i_sb < _session_data.n_task_send_at_once; i_sb++) {
        cham_migratable_task_t* cur_task = _remote_tasks_send_back.pop_front();
        if(cur_task) {
            action_send_back_stolen_tasks(cur_task, &request_manager_send);
        }
        else {
            break;
        }
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

    MPI_Status cur_status_activate;
    int flag_open_request_activate = 0;

    #if ENABLE_TASK_MIGRATION || CHAM_REPLICATION_MODE>0
    #if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
    cur_time = omp_get_wtime();
    #endif
    MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm, &flag_open_request_receive, &cur_status_receive);
    #if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
    cur_time = omp_get_wtime()-cur_time;
    if(cur_time>CHAM_SLOW_COMMUNICATION_THRESHOLD)
        _num_slow_communication_operations++;
    #endif
    
    #if CHAM_REPLICATION_MODE>=2
    MPI_Iprobe(MPI_ANY_SOURCE, 0, chameleon_comm_cancel, &flag_open_request_cancel, &cur_status_cancel);
    if( flag_open_request_cancel ) {
        action_handle_cancel_request(&cur_status_cancel);
    }
    #endif

    #if CHAM_REPLICATION_MODE==4
    MPI_Iprobe(MPI_ANY_SOURCE, 0, chameleon_comm_activate, &flag_open_request_activate, &cur_status_activate);
    if( flag_open_request_activate ) {
    	action_handle_activate_request(&cur_status_activate);
    }
    #endif

    #endif /* ENABLE_TASK_MIGRATION */

    if ( flag_open_request_receive ) {

        // check last recv id for source rank
        int last_id_handled = _session_data.tracked_last_req_recv[cur_status_receive.MPI_SOURCE];
        bool handle_req = last_id_handled != cur_status_receive.MPI_TAG;

        // only handle request if not already done for current phase
        if (handle_req) {
            // track last id recevied for source rank
            _session_data.tracked_last_req_recv[cur_status_receive.MPI_SOURCE] = cur_status_receive.MPI_TAG;
            // create requests to receive task
            action_handle_recv_request(&cur_status_receive, &request_manager_receive);
        }
    }

    #if CHAM_REPLICATION_MODE>0 || ENABLE_EARLY_IRECVS==0
    for (int i_sb = 0; i_sb < _session_data.n_task_send_at_once; i_sb++) {
        flag_open_request_receiveBack = 0;
        #if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
        cur_time = omp_get_wtime();
        #endif
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm_mapped, &flag_open_request_receiveBack, &cur_status_receiveBack);
        #if CHAM_STATS_RECORD && SHOW_WARNING_SLOW_COMMUNICATION
        cur_time = omp_get_wtime()-cur_time;
        if(cur_time>CHAM_SLOW_COMMUNICATION_THRESHOLD)
        _num_slow_communication_operations++;
        #endif
        if( flag_open_request_receiveBack ) {
            action_handle_recvback_request(&cur_status_receiveBack, &request_manager_receive);
        } else {
            break;
        }
    }
    #endif
    #if COMMUNICATION_MODE == 2
    _mtx_comm_progression.unlock();
    }
    #elif COMMUNICATION_MODE != 1
    }
    #endif
}

void* comm_thread_action(void* arg) {
    #if COMMUNICATION_MODE != 4
    pin_thread_to_last_core(1);
    #endif
    // trigger signal to tell that thread is running now
    pthread_mutex_lock( &_th_service_actions_mutex );
    _th_service_actions_created = 1;
    pthread_cond_signal( &_th_service_actions_cond );
    pthread_mutex_unlock( &_th_service_actions_mutex );

    DBP("comm_thread_action (enter)\n");

    #if CHAM_STATS_RECORD
    _session_data.time_start_comm = omp_get_wtime();
    #endif

    while(true) {
        #if THREAD_ACTIVATION
        while (_flag_comm_thread_sleeping) {
            // TODO: not 100% sure if we need that duplication, but it also doesn't hurt much in terms of performance as only happening in comm thread idle state
            if(!_session_data.flag_thread_sleeping_set) {
                _session_data.flag_thread_sleeping_set = 1;
                #if CHAM_STATS_RECORD
                double time_commthread_elapsed = omp_get_wtime()-_session_data.time_start_comm;
                atomic_add_dbl(_time_commthread_active_sum, time_commthread_elapsed);
                _time_commthread_active_count++;
                assert(_num_active_communications_overall.load()==0);
                #endif /* CHAM_STATS_RECORD */
                assert(_remote_tasks_send_back.empty());
                //if(request_manager_cancel.getNumberOfOutstandingRequests()!=0)
                //  printf("outstanding cancel %d\n",request_manager_cancel.getNumberOfOutstandingRequests());

                assert(request_manager_cancel.getNumberOfOutstandingRequests()==0);
                
                // run routine to cleanup all things for current work phase
                cleanup_work_phase();

                DBP("comm_thread_action - thread went to sleep again (inside while) - _comm_thread_service_stopped=%d\n", _comm_thread_service_stopped.load());
            }
            // dont do anything if the thread is sleeping
            usleep(COMM_THREAD_SLEEP_TIME_MICRO_SECS);
            if(_flag_abort_comm_thread) {
                DBP("comm_thread_action (abort)\n");
                if(_session_data.buffer_load_values) {
                    free(_session_data.buffer_load_values);
                    _session_data.buffer_load_values = nullptr;
                }
                int ret_val = 0;
                pthread_exit(&ret_val);
            }
        }
        if(_session_data.flag_thread_sleeping_set) {
            DBP("comm_thread_action - woke up again\n");
            #if CHAM_STATS_RECORD
            _session_data.time_start_comm = omp_get_wtime();
            #endif
            _session_data.flag_thread_sleeping_set = 0;
            tag_counter_send_tasks = 0;
            _session_data.has_replicated = false;
            _session_data.is_migration_victim = false;
            assert(_remote_tasks_send_back.empty());
            assert(request_manager_cancel.getNumberOfOutstandingRequests()==0);
            _cancelled_task_ids.clear();
            _map_offloaded_tasks_with_outputs.clear();
            // reset last received ids
            for(int tmp_i = 0; tmp_i < chameleon_comm_size; tmp_i++)
                _session_data.tracked_last_req_recv[tmp_i] = -1;
            _session_data.num_threads_in_tw = _num_threads_involved_in_taskwait.load();
        }
        #endif
#if defined(TRACE) && ENABLE_TRACING_FOR_SYNC_CYCLES
        if(_tracing_enabled) {
            VT_traceon();
        } else {
            VT_traceoff();
        }
#endif /* ENABLE_TRACING_FOR_SYNC_CYCLES */
        // call function for communication progression
        action_communication_progression(1);
        usleep(COMM_THREAD_SLEEP_TIME_MICRO_SECS);
    }
}
#pragma endregion CommThread

#ifdef __cplusplus
}
#endif
