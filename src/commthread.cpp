#include "commthread.h"
#include <pthread.h>
#include <signal.h>
#include <numeric>
#include <sched.h>
#include <hwloc.h>
#include <omp.h>
#include <algorithm>
#include <unordered_map>

#include "cham_statistics.h"

#ifdef TRACE
#include "VT.h"
#endif 

// communicator for remote task requests
MPI_Comm chameleon_comm;
// communicator for sending back mapped values
MPI_Comm chameleon_comm_mapped;
// communicator for load information
MPI_Comm chameleon_comm_load;

int chameleon_comm_rank = -1;
int chameleon_comm_size = -1;

// global counter for offloads used for generating unique tag id
std::mutex _mtx_global_offload_counter;
int _global_offload_counter = 0;

std::vector<intptr_t> _image_base_addresses;

// list with data that has been mapped in map clauses
std::mutex _mtx_data_entry;
std::list<OffloadingDataEntryTy*> _data_entries;

// list with local task entries
// these can either be executed here or offloaded to a different rank
std::mutex _mtx_local_tasks;
std::list<TargetTaskEntryTy*> _local_tasks;
int32_t _num_local_tasks_outstanding = 0;

// list with stolen task entries that should be executed
std::mutex _mtx_stolen_remote_tasks;
std::list<TargetTaskEntryTy*> _stolen_remote_tasks;
int32_t _num_stolen_tasks_outstanding = 0;

// list with stolen task entries that need output data transfer
std::mutex _mtx_stolen_remote_tasks_send_back;
std::list<TargetTaskEntryTy*> _stolen_remote_tasks_send_back;

// entries that should be offloaded to specific ranks
std::mutex _mtx_offload_entries;
std::list<OffloadEntryTy*> _offload_entries;

// map that maps tag id's back to local tasks that have been offloaded
std::mutex _mtx_map_tag_to_task;
std::unordered_map<int, TargetTaskEntryTy*> _map_tag_to_task;

// ====== Info about outstanding jobs (local & stolen) ======
// extern std::mutex _mtx_outstanding_jobs;
std::vector<int32_t> _outstanding_jobs_ranks;
int32_t _outstanding_jobs_local;
int32_t _outstanding_jobs_sum;
// ====== Info about real load that is open or is beeing processed ======
// extern std::mutex _mtx_load_info;
std::vector<int32_t> _load_info_ranks;
int32_t _load_info_local;
int32_t _load_info_sum;
// for now use a single mutex for box info
std::mutex _mtx_load_exchange;

#if OFFLOAD_BLOCKING
// only enable offloading when a task has finished on local rank (change has been made)
std::mutex _mtx_offload_blocked;
int32_t _offload_blocked = 0;
#endif

// === Constants
const int32_t MAX_BUFFER_SIZE_OFFLOAD_ENTRY = 20480; // 20 KB for testing

// ============== Thread Section ===========
std::mutex _mtx_comm_threads_started;
int _comm_threads_started               = 0;
int _comm_thread_load_exchange_happend  = 0;

std::mutex _mtx_comm_threads_ended;
int _comm_threads_ended_count           = 0;

// flag that signalizes comm threads to abort their work
int _flag_abort_threads         = 0;

pthread_t           _th_receive_remote_tasks;
int                 _th_receive_remote_tasks_created = 0;
pthread_cond_t      _th_receive_remote_tasks_cond    = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     _th_receive_remote_tasks_mutex   = PTHREAD_MUTEX_INITIALIZER;

pthread_t           _th_service_actions;
int                 _th_service_actions_created = 0;
pthread_cond_t      _th_service_actions_cond    = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     _th_service_actions_mutex   = PTHREAD_MUTEX_INITIALIZER;


template <typename T>
std::vector<size_t> sort_indexes(const std::vector<T> &v) {

  // initialize original index locations
  std::vector<size_t> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);

  // sort indexes based on comparing values in v
  std::sort(idx.begin(), idx.end(),
       [&v](size_t i1, size_t i2) {return v[i1] < v[i2];});

  return idx;
}

#ifdef __cplusplus
extern "C" {
#endif
// ================================================================================
// Forward declartion of internal functions (just called inside shared library)
// ================================================================================
void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size);
void * encode_send_buffer_metadata(TargetTaskEntryTy *task, int32_t *buffer_size, int tag);
TargetTaskEntryTy* decode_send_buffer(void * buffer);
TargetTaskEntryTy* decode_send_buffer_metadata(void * buffer);
short pin_thread_to_last_core();
void* offload_action(OffloadEntryTy *task);

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

#if CHAM_STATS_RECORD
    cham_stats_init_stats();
#endif

    DBP("start_communication_threads (enter)\n");
    // set flag to avoid that threads are directly aborting
    _flag_abort_threads = 0;
    _comm_thread_load_exchange_happend = 0;
    _global_offload_counter = 0;    

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
    
    // // wait for finished thread creation
    // pthread_mutex_lock(&_th_receive_remote_tasks_mutex);
    // while (_th_receive_remote_tasks_created == 0) {
    //     pthread_cond_wait(&_th_receive_remote_tasks_cond, &_th_receive_remote_tasks_mutex);
    // }
    // pthread_mutex_unlock(&_th_receive_remote_tasks_mutex);

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

// only last openmp thread should be able to stop threads
int32_t stop_communication_threads() {
    _mtx_comm_threads_ended.lock();
    // increment counter
    _comm_threads_ended_count++;

    // check whether it is the last thread that comes along here
    // TODO: omp_get_num_threads might not be the correct choice in all cases.. need to check
    if(_comm_threads_ended_count < omp_get_num_threads()) {
        _mtx_comm_threads_ended.unlock();
        return CHAM_SUCCESS;
    }

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
    _comm_thread_load_exchange_happend = 0;
    _comm_threads_ended_count = 0;
    _global_offload_counter = 0;
#ifdef CHAM_DEBUG
    DBP("stop_communication_threads - still mem_allocated = %ld\n", (long)mem_allocated);
    mem_allocated = 0;
#endif

    _th_receive_remote_tasks_created = 0;
    _th_service_actions_created = 0;

#if CHAM_STATS_RECORD && CHAM_STATS_PRINT
    cham_stats_print_stats();
#endif
    DBP("stop_communication_threads (exit)\n");
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

int32_t offload_task_to_rank(OffloadEntryTy *entry) {
#ifdef TRACE
    static int event_offload = -1;
    std::string event_offload_name = "offload_task";
    if(event_offload == -1) 
        int ierr = VT_funcdef(event_offload_name.c_str(), VT_NOCLASS, &event_offload);
    VT_begin(event_offload);
#endif 
    int has_outputs = entry->task_entry->HasAtLeastOneOutput();
    DBP("offload_task_to_rank (enter) - task_entry: " DPxMOD ", num_args: %d, rank: %d, has_output: %d\n", DPxPTR(entry->task_entry->tgt_entry_ptr), entry->task_entry->arg_num, entry->target_rank, has_outputs);

    _mtx_load_exchange.lock();
    _load_info_local--;
    trigger_update_outstanding();
    _mtx_load_exchange.unlock();

    // explicitly make threads joinable to be portable
    //pthread_attr_t attr;
    //pthread_attr_init(&attr);
    //pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    // TODO: might be better to not create additional thread if there are output variables
    // if(entry->HasAtLeastOneOutput()) {
    //    pthread_t *tmp_new_thread = (pthread_t*)malloc(sizeof(pthread_t));

    //    int err;
    //    err = pthread_create(tmp_new_thread, &attr, thread_offload_action, (void*)entry);
    //    if(err != 0)
    //        handle_error_en(err, "offload_task_to_rank - pthread_create");
    // } else {
        // ...
    // }

    offload_action( entry);

#if CHAM_STATS_RECORD
    _mtx_num_tasks_offloaded.lock();
    _num_tasks_offloaded++;
    _mtx_num_tasks_offloaded.unlock();
#endif

    DBP("offload_task_to_rank (exit)\n");
#ifdef TRACE
    VT_end(event_offload);
#endif
    return CHAM_SUCCESS;
}

// This should run in a single pthread because it should be blocking
void* offload_action(OffloadEntryTy *entry) {
#ifdef TRACE
    static int event_offload_send = -1;
    std::string event_offload_send_name = "offload_send";
    if(event_offload_send == -1) 
        int ierr = VT_funcdef(event_offload_send_name.c_str(), VT_NOCLASS, &event_offload_send);

     VT_begin(event_offload_send);
#endif

    int has_outputs = entry->task_entry->HasAtLeastOneOutput();
    DBP("offload_action (enter) - task_entry: " DPxMOD ", num_args: %d, rank: %d, has_output: %d\n", DPxPTR(entry->task_entry->tgt_entry_ptr), entry->task_entry->arg_num, entry->target_rank, has_outputs);

    // calculate proper tag that contains bit combination of sender as well as 
    // "task id" (maybe global increment per process)
    _mtx_global_offload_counter.lock();
    int tmp_counter = ++_global_offload_counter;
    _mtx_global_offload_counter.unlock();
    int tmp_rank = chameleon_comm_rank;
    int tmp_tag = (tmp_rank << 16) | (tmp_counter);

    // encode buffer
    int32_t buffer_size = 0;
    void * buffer = encode_send_buffer_metadata(entry->task_entry, &buffer_size, tmp_tag);

#ifdef TRACE
    VT_begin(event_offload_send);
#endif
    // // == DEBUG: Verify bit encoding
    // int tmp_verify_count = tmp_tag & 0x0000ffff;
    // int tmp_verify_rank = tmp_tag >> 16;
    // // == DEBUG
    
    // send data to target rank
    DBP("offload_action - sending data to target rank %d with tag: %d\n", entry->target_rank, tmp_tag);

#if CHAM_STATS_RECORD
    double cur_time;
    cur_time = omp_get_wtime();
#endif
    MPI_Send(buffer, buffer_size, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm); //TODO: introduce metadata_tag

    for(int i=0; i<entry->task_entry->arg_num; i++) {
        int is_lit      = entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        if(is_lit) {
            MPI_Send(&entry->task_entry->arg_hst_pointers[i], entry->task_entry->arg_sizes[i], MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm);
        }
        else{
            MPI_Send(entry->task_entry->arg_hst_pointers[i], entry->task_entry->arg_sizes[i], MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm);
        } 
   }
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    _mtx_time_comm_send_task.lock();
    _time_comm_send_task_sum += cur_time;
    _time_comm_send_task_count++;
    _mtx_time_comm_send_task.unlock();
#endif
    free(buffer);

#ifdef TRACE
    VT_end(event_offload_send);
#endif

    if(has_outputs) {
        _mtx_map_tag_to_task.lock();
        _map_tag_to_task.insert(std::make_pair(tmp_tag, entry->task_entry));
        _mtx_map_tag_to_task.unlock();
    }

    // decrement counter if offloading + receiving results finished
    _mtx_load_exchange.lock();
    _num_local_tasks_outstanding--;
    trigger_update_outstanding();
    _mtx_load_exchange.unlock();

    DBP("offload_action (exit)\n");
    return nullptr;
}

void * encode_send_buffer_metadata(TargetTaskEntryTy *task, int32_t *buffer_size, int tag) {
#ifdef TRACE
    static int event_encode = -1;
    std::string event_encode_name = "encode";
    if(event_encode == -1) 
        int ierr = VT_funcdef(event_encode_name.c_str(), VT_NOCLASS, &event_encode);
    VT_begin(event_encode);
#endif 

    DBP("encode_send_buffer_metadata (enter) - task_entry: " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

    // FORMAT:
    //      1. target function pointer = address (intptr_t)
    //      2. image index
    //      3. offset of entry point inside image
    //      4. number of arguments = int32_t
    //      5. array with argument types = n_args * int64_t
    //      6. array with argument offsets = n_args * int64_t
    //      7. array with length of argument pointers = n_args * int64_t
   int total_size = sizeof(intptr_t)           // 1. target entry pointer
        + sizeof(int32_t)                       // 2. img index
        + sizeof(ptrdiff_t)                     // 3. offset inside image
        + sizeof(int32_t)                       // 4. number of arguments
        + task->arg_num * sizeof(int64_t)       // 5. argument sizes
        + task->arg_num * sizeof(ptrdiff_t)     // 6. offsets
        + task->arg_num * sizeof(int64_t);       // 7. argument types
	 
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

   
    // set output size
    *buffer_size = total_size;
#ifdef TRACE
    VT_end(event_encode);
#endif
    return buff;    
}

void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size) {
#ifdef TRACE
    static int event_encode = -1;
    std::string event_encode_name = "encode";
    if(event_encode == -1) 
        int ierr = VT_funcdef(event_encode_name.c_str(), VT_NOCLASS, &event_encode);
    VT_begin(event_encode);
#endif 
    DBP("encode_send_buffer (enter) - task_entry: " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

    // FORMAT:
    //      1. target function pointer = address (intptr_t)
    //      2. image index
    //      3. offset of entry point inside image
    //      4. number of arguments = int32_t
    //      5. array with argument types = n_args * int64_t
    //      6. array with argument offsets = n_args * int64_t
    //      7. array with length of argument pointers = n_args * int64_t
    //      8. array with values

    // TODO: Is it worth while to consider MPI packed data types??

    int total_size = sizeof(intptr_t)           // 1. target entry pointer
        + sizeof(int32_t)                       // 2. img index
        + sizeof(ptrdiff_t)                     // 3. offset inside image
        + sizeof(int32_t)                       // 4. number of arguments
        + task->arg_num * sizeof(int64_t)       // 5. argument sizes
        + task->arg_num * sizeof(ptrdiff_t)     // 6. offsets
        + task->arg_num * sizeof(int64_t);      // 7. argument types

    for(int i = 0; i < task->arg_num; i++) {
        total_size += task->arg_sizes[i];
    }

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

    // set output size
    *buffer_size = total_size;
#ifdef TRACE
    VT_end(event_encode);
#endif
    return buff;    
}

TargetTaskEntryTy* decode_send_buffer(void * buffer) {
#ifdef TRACE
    static int event_decode = -1;
    std::string event_decode_name = "decode";
    if(event_decode == -1) 
        int ierr = VT_funcdef(event_decode_name.c_str(), VT_NOCLASS, &event_decode);
    VT_begin(event_decode);
#endif 
    // init new task
    TargetTaskEntryTy* task = new TargetTaskEntryTy();

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

    DBP("decode_send_buffer (enter) - task_entry: " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

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
        
        print_arg_info("decode_send_buffer", task, i);

        cur_ptr += task->arg_sizes[i];
    }
#ifdef TRACE
    VT_end(event_decode);
#endif 
    return task;
}

TargetTaskEntryTy* decode_send_buffer_metadata(void * buffer) {
#ifdef TRACE
    static int event_decode = -1;
    std::string event_decode_name = "decode";
    if(event_decode == -1) 
        int ierr = VT_funcdef(event_decode_name.c_str(), VT_NOCLASS, &event_decode);
    VT_begin(event_decode);
#endif 
    // init new task
    TargetTaskEntryTy* task = new TargetTaskEntryTy();

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

    DBP("decode_send_buffer (enter) - task_entry: " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

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

    for(int32_t i = 0; i < task->arg_num; i++) {
        int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;

        // copy value from host pointer directly
        if(!is_lit) {
            // need to allocate new memory
            void * new_mem = malloc(task->arg_sizes[i]);
            task->arg_hst_pointers[i] = new_mem;
        }
        print_arg_info("decode_send_buffer", task, i);
    }
#ifdef TRACE
    VT_end(event_decode);
#endif 
    return task;
}

//int decode_tag_from_metadata(void *buffer, int32_t bufferSize) {
//    char *cur_ptr = (char*) buffer;
//    cur_ptr += bufferSize;
//    cur_ptr -= sizeof(int);
//    return ((int) cur_ptr[0]);
//}


// should run in a single thread that is always waiting for incoming requests
void* receive_remote_tasks(void* arg) {
#ifdef TRACE
    static int event_receive_tasks = -1;
    std::string event_receive_tasks_name = "receive_task";
    if(event_receive_tasks == -1) 
        int ierr = VT_funcdef(event_receive_tasks_name.c_str(), VT_NOCLASS, &event_receive_tasks);

    static int event_recv_back = -1;
    std::string event_recv_back = "receive_back";
    if(event_offload_recv == -1) 
        int ierr = VT_funcdef(event_recv_back.c_str(), VT_NOCLASS, &event_recv_back);
#endif 
    // pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
    // pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pin_thread_to_last_core();
    
    // // trigger signal to tell that thread is running now
    // pthread_mutex_lock( &_th_receive_remote_tasks_mutex );
    // _th_receive_remote_tasks_created = 1; 
    // pthread_cond_signal( &_th_receive_remote_tasks_cond );
    // pthread_mutex_unlock( &_th_receive_remote_tasks_mutex );

    DBP("receive_remote_tasks (enter)\n");

    int32_t res;
    // intention to reuse buffer over and over again
    int cur_max_buff_size = MAX_BUFFER_SIZE_OFFLOAD_ENTRY;
    void * buffer = malloc(cur_max_buff_size);
    int recv_buff_size = 0;
 
    double cur_time;
    
    while(true) {
        // first check transmission and make sure that buffer has enough memory
        MPI_Status cur_status_receive;
        MPI_Status cur_status_receiveBack;
        int flag_open_request_receive = 0;
        int flag_open_request_receiveBack = 0;      

        while(!flag_open_request_receive && !flag_open_request_receiveBack) {
            usleep(5);
            // check whether thread should be aborted
            if(_flag_abort_threads) {
                DBP("receive_remote_tasks (abort)\n");
                free(buffer);
                int ret_val = 0;
                pthread_exit(&ret_val);
            }
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm, &flag_open_request_receive, &cur_status_receive);
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm_mapped, &flag_open_request_receiveBack, &cur_status_receiveBack);
        }

        if( flag_open_request_receiveBack ) {
	        _mtx_map_tag_to_task.lock();
            std::unordered_map<int ,TargetTaskEntryTy*>::const_iterator got = _map_tag_to_task.find(cur_status_receiveBack.MPI_TAG);
	        _mtx_map_tag_to_task.unlock();
            // receive data back
	        if(got!=_map_tag_to_task.end()) {
	            MPI_Get_count(&cur_status_receiveBack, MPI_BYTE, &recv_buff_size);
                if(recv_buff_size > cur_max_buff_size) {
                    // allocate more memory
                    free(buffer);
                    cur_max_buff_size = recv_buff_size;
                    buffer = malloc(recv_buff_size);
                }
#ifdef TRACE
                VT_begin(event_recv_back);
#endif
 
#if CHAM_STATS_RECORD
                cur_time = omp_get_wtime();
#endif
                MPI_Recv(buffer, recv_buff_size, MPI_BYTE, cur_status_receiveBack.MPI_SOURCE, cur_status_receiveBack.MPI_TAG,
                                                                                      chameleon_comm_mapped, MPI_STATUS_IGNORE);
#if CHAM_STATS_RECORD
                cur_time = omp_get_wtime()-cur_time;
                _mtx_time_comm_back_recv.lock();
                _time_comm_back_recv_sum += cur_time;
                _time_comm_back_recv_count++;
                _mtx_time_comm_back_recv.unlock();
#endif
                DBP("receive_remote_tasks - receiving output data from rank %d for tag: %d\n", cur_status_receiveBack.MPI_SOURCE, 
                                                                                               cur_status_receiveBack.MPI_TAG);
                // copy results back to source pointers with memcpy
                TargetTaskEntryTy *task_entry = got->second;
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
#ifdef TRACE
                VT_end(event_offload_wait_recv);
#endif
            }
	    }
        // receive new task
        if ( flag_open_request_receive ) {
#if CHAM_STATS_RECORD
            cur_time = omp_get_wtime();
#endif
#ifdef TRACE
            VT_begin(event_receive_tasks);
#endif
            MPI_Get_count(&cur_status_receive, MPI_BYTE, &recv_buff_size);
            if(recv_buff_size > cur_max_buff_size) {
                // allocate more memory
                free(buffer);
                cur_max_buff_size = recv_buff_size;
                buffer = malloc(recv_buff_size);
            }

            // now receive the metadata
            res = MPI_Recv(buffer, recv_buff_size, MPI_BYTE, cur_status_receive.MPI_SOURCE, cur_status_receive.MPI_TAG, chameleon_comm, MPI_STATUS_IGNORE);
#ifdef TRACE
            VT_end(event_receive_tasks);
#endif
	        TargetTaskEntryTy *task = decode_send_buffer_metadata(buffer);
#ifdef TRACE
            VT_begin(event_receive_tasks);
#endif
	        for(int32_t i=0; i<task->arg_num; i++) {
                int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
                if(is_lit) {
                    MPI_Recv(&task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, cur_status_receive.MPI_SOURCE, cur_status_receive.MPI_TAG, chameleon_comm, MPI_STATUS_IGNORE);
                } else {
	                MPI_Recv(task->arg_hst_pointers[i], task->arg_sizes[i], MPI_BYTE, cur_status_receive.MPI_SOURCE, cur_status_receive.MPI_TAG, chameleon_comm, MPI_STATUS_IGNORE);
                }
            } 
#ifdef TRACE
            VT_end(event_receive_tasks);
#endif

#if CHAM_STATS_RECORD
            cur_time = omp_get_wtime()-cur_time;
            _mtx_time_comm_recv_task.lock();
            _time_comm_recv_task_sum += cur_time;
            _time_comm_recv_task_count++;
            _mtx_time_comm_recv_task.unlock();
#endif

            // set information for sending back results/updates if necessary
            task->source_mpi_rank   = cur_status_receive.MPI_SOURCE;
            task->source_mpi_tag    = cur_status_receive.MPI_TAG;

            // add task to stolen list and increment counter
            _mtx_stolen_remote_tasks.lock();
            _stolen_remote_tasks.push_back(task);
            _mtx_stolen_remote_tasks.unlock();

            _mtx_load_exchange.lock();
            _num_stolen_tasks_outstanding++;
            _load_info_local++;
            trigger_update_outstanding();
            _mtx_load_exchange.unlock();
        }
    }
}

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

    int transported_load_values[2];
    int * buffer_load_values = (int*) malloc(sizeof(int)*2*chameleon_comm_size);

    DBP("service_thread_action (enter)\n");
    while(true) {
        TargetTaskEntryTy* cur_task = nullptr;

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
            transported_load_values[0] = _outstanding_jobs_local;
            transported_load_values[1] = _load_info_local;
            _mtx_load_exchange.unlock();
            MPI_Iallgather(&transported_load_values[0], 2, MPI_INT, buffer_load_values, 2, MPI_INT, chameleon_comm_load, &request_out);
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
            int32_t sum_outstanding = 0;
            int32_t sum_load = 0;
            for(int j = 0; j < chameleon_comm_size; j++) {
                _outstanding_jobs_ranks[j]  = buffer_load_values[j*2];
                _load_info_ranks[j]         = buffer_load_values[(j*2)+1];
                sum_outstanding             += _outstanding_jobs_ranks[j];
                sum_load                    += _load_info_ranks[j];
                // DBP("load info from rank %d = %d\n", j, _load_info_ranks[j]);
            }
            _outstanding_jobs_sum           = sum_outstanding;
            _load_info_sum                  = sum_load;
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
        }

        // check whether to abort thread
        if(_flag_abort_threads) {
            DBP("service_thread_action (abort)\n");
            free(buffer_load_values);
            int ret_val = 0;
            pthread_exit(&ret_val);
        }

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
            
            // check other ranks whether there is a rank with low load; start at current position
            for(int k = 1; k < chameleon_comm_size; k++) {
                int tmp_idx = (chameleon_comm_rank + k) % chameleon_comm_size;
                if(_load_info_ranks[tmp_idx] == 0) {
                    // Direct offload if thread found that has nothing to do
                    TargetTaskEntryTy *cur_task = chameleon_pop_task();
                    if(cur_task == nullptr)
                        break;
#ifdef TRACE
            VT_end(event_offload_decision);
#endif
                    DBP("OffloadingDecision: MyLoad: %d, Rank %d is empty, LastKnownSumOutstandingJobs: %d\n", cur_load, tmp_idx, last_known_sum_outstanding);
                    OffloadEntryTy * off_entry = new OffloadEntryTy(cur_task, tmp_idx);
                    offload_task_to_rank(off_entry);
                    offload_triggered = 1;

#if OFFLOAD_BLOCKING
                    _mtx_offload_blocked.lock();
                    _offload_blocked = 1;
                    _mtx_offload_blocked.unlock();
#endif
                    break;
                }
            }
            // only proceed if offloading not already performed
            if(!offload_triggered) {
                std::vector<size_t> tmp_sorted_idx = sort_indexes(_load_info_ranks);
                int min_val = _load_info_ranks[tmp_sorted_idx[0]];
                int max_val = _load_info_ranks[tmp_sorted_idx[chameleon_comm_size-1]];
                if(max_val > min_val) {
                    // determine index
                    int pos = std::find(tmp_sorted_idx.begin(), tmp_sorted_idx.end(), chameleon_comm_rank) - tmp_sorted_idx.begin();
                    // only offload if on the upper side
                    if((pos+1) >= ((double)chameleon_comm_size/2.0))
                    {
                        int other_pos = chameleon_comm_size-pos;
                        // need to adapt in case of even number
                        if(chameleon_comm_size % 2 == 0)
                            other_pos--;
                        int other_idx = tmp_sorted_idx[other_pos];
                        int other_val = _load_info_ranks[other_idx];

                        // calculate ration between those two and just move if over a certain threshold
                        double ratio = (double)(cur_load-other_val) / (double)cur_load;
                        if(other_val < cur_load && ratio > 0.5) {
                            TargetTaskEntryTy *cur_task = chameleon_pop_task();
                            if(cur_task) {
#ifdef TRACE
                                VT_end(event_offload_decision);
#endif
                                DBP("OffloadingDecision: MyLoad: %d, Load Rank %d is %d, LastKnownSumOutstandingJobs: %d\n", cur_load, other_idx, other_val, last_known_sum_outstanding);
                                OffloadEntryTy * off_entry = new OffloadEntryTy(cur_task, other_idx);
                                offload_task_to_rank(off_entry);
                                offload_triggered = 1;

#if OFFLOAD_BLOCKING
                                _mtx_offload_blocked.lock();
                                _offload_blocked = 1;
                                _mtx_offload_blocked.unlock();
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
#endif

        // ================= Sending back results for stolen tasks =================
        if(_stolen_remote_tasks_send_back.empty()) {
            usleep(5);
            continue;
        }

        _mtx_stolen_remote_tasks_send_back.lock();
        // need to check again
        if(_stolen_remote_tasks_send_back.empty()) {
            _mtx_stolen_remote_tasks_send_back.unlock();
            continue;
        }
        cur_task = _stolen_remote_tasks_send_back.front();
        _stolen_remote_tasks_send_back.pop_front();
        _mtx_stolen_remote_tasks_send_back.unlock();

#ifdef TRACE
        VT_begin(event_send_back);
#endif

        int32_t tmp_size_buff = 0;
        for(int i = 0; i < cur_task->arg_num; i++) {
            if(cur_task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                tmp_size_buff += cur_task->arg_sizes[i];
            }
        }
        DBP("service_thread_action - sending back data to rank %d with tag %d\n", cur_task->source_mpi_rank, cur_task->source_mpi_tag);
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
        MPI_Send(buff, tmp_size_buff, MPI_BYTE, cur_task->source_mpi_rank, cur_task->source_mpi_tag, chameleon_comm_mapped);
#if CHAM_STATS_RECORD
        cur_time = omp_get_wtime()-cur_time;
        _mtx_time_comm_back_send.lock();
        _time_comm_back_send_sum += cur_time;
        _time_comm_back_send_count++;
        _mtx_time_comm_back_send.unlock();
#endif
        free(buff);

#ifdef TRACE
        VT_end(event_send_back);
#endif

        _mtx_load_exchange.lock();
        _num_stolen_tasks_outstanding--;
        trigger_update_outstanding();
        _mtx_load_exchange.unlock();
    }
}

void trigger_update_outstanding() {
    // _mtx_load_exchange.lock();
    _outstanding_jobs_local = _num_local_tasks_outstanding + _num_stolen_tasks_outstanding;
    DBP("trigger_update_outstanding - current oustanding jobs: %d, current_local_load = %d\n", _outstanding_jobs_local, _load_info_local);
    // _mtx_load_exchange.unlock();
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

#ifdef __cplusplus
}
#endif
