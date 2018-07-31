#include "commthread.h"
#include <pthread.h>
#include <signal.h>

#include <sched.h>
#include <hwloc.h>
#include <omp.h>

// communicator for remote task requests
MPI_Comm chameleon_comm;
// communicator for sending back mapped values
MPI_Comm chameleon_comm_mapped;
// communicator for load information
MPI_Comm chameleon_comm_load;

int chameleon_comm_rank = -1;
int chameleon_comm_size = -1;

std::vector<intptr_t> _image_base_addresses;

// list with data that has been mapped in map clauses
std::mutex _mtx_data_entry;
std::list<OffloadingDataEntryTy*> _data_entries;

// list with local task entries
// these can either be executed here or offloaded to a different rank
std::mutex _mtx_local_tasks;
std::list<TargetTaskEntryTy*> _local_tasks;
int32_t _num_local_tasks = 0;

// list with stolen task entries that should be executed
std::mutex _mtx_stolen_remote_tasks;
std::list<TargetTaskEntryTy*> _stolen_remote_tasks;
int32_t _num_stolen_tasks = 0;

// list with stolen task entries that need output data transfer
std::mutex _mtx_stolen_remote_tasks_send_back;
std::list<TargetTaskEntryTy*> _stolen_remote_tasks_send_back;

// entries that should be offloaded to specific ranks
std::mutex _mtx_offload_entries;
std::list<OffloadEntryTy*> _offload_entries;

// Counter what needs to be done locally
std::mutex _mtx_outstanding_local_jobs;
int32_t _outstanding_local_jobs = 0;

std::mutex _mtx_complete_load_info;
int32_t *_complete_load_info;
int32_t _sum_complete_load_info = 0;

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

pthread_t           _th_send_back_mapped_data;
int                 _th_send_back_mapped_data_created = 0;
pthread_cond_t      _th_send_back_mapped_data_cond    = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     _th_send_back_mapped_data_mutex   = PTHREAD_MUTEX_INITIALIZER;

#ifdef __cplusplus
extern "C" {
#endif
// ================================================================================
// Forward declartion of internal functions (just called inside shared library)
// ================================================================================
void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size);
TargetTaskEntryTy* decode_send_buffer(void * buffer);
short pin_thread_to_last_core();
void* thread_offload_action(void *arg);

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
    _flag_abort_threads = 0;
    _comm_thread_load_exchange_happend = 0;

    // explicitly make threads joinable to be portable
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    int err;
    err = pthread_create(&_th_receive_remote_tasks, &attr, receive_remote_tasks, NULL);
    if(err != 0)
        handle_error_en(err, "pthread_create - _th_receive_remote_tasks");
    err = pthread_create(&_th_send_back_mapped_data, &attr, send_back_mapped_data, NULL);
    if(err != 0)
        handle_error_en(err, "pthread_create - _th_send_back_mapped_data");
    
    // wait for finished thread creation
    pthread_mutex_lock(&_th_receive_remote_tasks_mutex);
    while (_th_receive_remote_tasks_created == 0) {
        pthread_cond_wait(&_th_receive_remote_tasks_cond, &_th_receive_remote_tasks_mutex);
    }
    pthread_mutex_unlock(&_th_receive_remote_tasks_mutex);

    pthread_mutex_lock(&_th_send_back_mapped_data_mutex);
    while (_th_send_back_mapped_data_created == 0) {
        pthread_cond_wait(&_th_send_back_mapped_data_cond, &_th_send_back_mapped_data_mutex);
    }
    pthread_mutex_unlock(&_th_send_back_mapped_data_mutex);

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
    _flag_abort_threads = 1;
    // then wait for all threads to finish
    err = pthread_join(_th_receive_remote_tasks, NULL);
    if(err != 0)    handle_error_en(err, "stop_communication_threads - _th_receive_remote_tasks");
    err = pthread_join(_th_send_back_mapped_data, NULL);
    if(err != 0)    handle_error_en(err, "stop_communication_threads - _th_send_back_mapped_data");

    // should be save to reset flags and counters here
    _comm_threads_started = 0;
    _comm_thread_load_exchange_happend = 0;
    _comm_threads_ended_count = 0;

    _th_receive_remote_tasks_created = 0;
    _th_send_back_mapped_data_created = 0;
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
            DBP("Last core/hw thread in cpuset is %ld\n", i);
            max_core_set = i;
            break;
        }
    }

    // set affinity mask to last core or all hw threads on specific core 
    CPU_ZERO(&new_cpu_set);
    if(max_core_set < n_physical_cores) {
        // Case: there are no hyper threads
        DBP("Setting thread affinity to core %ld\n", max_core_set);
        CPU_SET(max_core_set, &new_cpu_set);
    } else {
        // Case: there are at least 2 HT per core
        std::string cores(std::to_string(max_core_set));
        CPU_SET(max_core_set, &new_cpu_set);
        for(long i = max_core_set-n_physical_cores; i >= 0; i-=n_physical_cores) {
            cores = std::to_string(i)  + "," + cores;
            CPU_SET(i, &new_cpu_set);
        }
        DBP("Setting thread affinity to cores %s\n", cores.c_str());
    }
    err = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &new_cpu_set);
    if (err != 0)
        handle_error_en(err, "pthread_setaffinity_np");

    // ===== DEBUG
    // verify that pinning worked
    err = pthread_getaffinity_np(thread, sizeof(cpu_set_t), &final_cpu_set);
    if (err != 0)
        handle_error_en(err, "pthread_getaffinity_np");

    std::string final_cores("");
    for (int j = 0; j < n_logical_cores; j++)
        if (CPU_ISSET(j, &final_cpu_set))
            final_cores += std::to_string(j) + ",";

    final_cores.pop_back();
    DBP("Verifying thread affinity: pinned to cores %s\n", final_cores.c_str());
    // ===== DEBUG

    return CHAM_SUCCESS;
}
#pragma endregion Start/Stop/Pin Communication Threads

int32_t offload_task_to_rank(OffloadEntryTy *entry) {
    // explicitly make threads joinable to be portable
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    // TODO: might be better to not create additional thread if there are output variables

    pthread_t *tmp_new_thread = (pthread_t*)malloc(sizeof(pthread_t));

    int err;
    err = pthread_create(tmp_new_thread, &attr, thread_offload_action, (void*)entry);
    if(err != 0)
        handle_error_en(err, "offload_task_to_rank - pthread_create");
    return CHAM_SUCCESS;
}

// This should run in a single pthread because it should be blocking
void* thread_offload_action(void *arg) {
    pin_thread_to_last_core();

    OffloadEntryTy *entry = (OffloadEntryTy *) arg;
    int has_outputs = entry->task_entry->HasAtLeastOneOutput();
    DBP("thread_offload_action (enter) - task_entry: " DPxMOD ", num_args: %d, rank: %d, has_output: %d\n", DPxPTR(entry->task_entry->tgt_entry_ptr), entry->task_entry->arg_num, entry->target_rank, has_outputs);

    // encode buffer
    int32_t buffer_size = 0;
    void * buffer = encode_send_buffer(entry->task_entry, &buffer_size);

    // TODO: calculate proper tag that contains bit combination of sender as well as 
    // "task id" (maybe global increment per process)
    int tmp_tag = 0;
    
    // send data to target rank
    DBP("thread_offload_action - sending data to target rank %d with tag: %d\n", entry->target_rank, tmp_tag);

    MPI_Send(buffer, buffer_size, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm);
    free(buffer);

    if(has_outputs) {
        DBP("thread_offload_action - waiting for output data from rank %d for tag: %d\n", entry->target_rank, tmp_tag);
        // temp buffer to retreive output data from target rank to be able to update host pointers again
        void * temp_buffer = malloc(MAX_BUFFER_SIZE_OFFLOAD_ENTRY);
        MPI_Status cur_status;
        MPI_Recv(temp_buffer, MAX_BUFFER_SIZE_OFFLOAD_ENTRY, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm_mapped, &cur_status);

        DBP("thread_offload_action - receiving output data from rank %d for tag: %d\n", cur_status.MPI_SOURCE, cur_status.MPI_TAG);
        // copy results back to source pointers with memcpy
        char * cur_ptr = (char*) temp_buffer;
        for(int i = 0; i < entry->task_entry->arg_num; i++) {
            int is_lit      = entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_from     = entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;

            if(entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                DBP("thread_offload_action - arg: " DPxMOD ", size: %ld, type: %ld, literal: %d, from: %d\n", 
                    DPxPTR(entry->task_entry->arg_hst_pointers[i]), 
                    entry->task_entry->arg_sizes[i],
                    entry->task_entry->arg_types[i],
                    is_lit,
                    is_from);
                    
                // we already have information about size and data type
                memcpy(entry->task_entry->arg_hst_pointers[i], cur_ptr, entry->task_entry->arg_sizes[i]);
                cur_ptr += entry->task_entry->arg_sizes[i];
            }
        }
        // free buffer again
        free(temp_buffer);
    }

    // decrement counter if offloading + receiving results finished
    _mtx_local_tasks.lock();
    _num_local_tasks--;
    trigger_local_load_update();
    _mtx_local_tasks.unlock();

    DBP("thread_offload_action (exit)\n");

    return nullptr;
    // int ret_val = 0;
    // pthread_exit(&ret_val);
}

void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size) {
    DBP("encode_send_buffer (enter) - task_entry: " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

    // FORMAT:
    //      1. target function pointer = address (intptr_t)
    //      2. image index
    //      3. offset of entry point inside image
    //      4. number of arguments = int32_t
    //      5. array with argument types = n_args * int64_t
    //      6. array with length of argument pointers = n_args * int64_t
    //      7. array with values

    // TODO: Is it worth while to consider MPI packed data types??

    int total_size = sizeof(intptr_t)           // 1. target entry pointer
        + sizeof(int32_t)                       // 2. img index
        + sizeof(ptrdiff_t)                     // 3. offset inside image
        + sizeof(int32_t)                       // 4. number of arguments
        + task->arg_num * sizeof(int64_t)       // 5. argument sizes
        + task->arg_num * sizeof(int64_t);      // 6. argument types

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

    // 6. argument types
    memcpy(cur_ptr, &(task->arg_types[0]), task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 7. loop through arguments and copy values
    for(int32_t i = 0; i < task->arg_num; i++) {
        int is_lit      = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        int is_from     = task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;

        DBP("encode_send_buffer - arg: " DPxMOD ", size: %ld, type: %ld, literal: %d, from: %d\n", 
            DPxPTR(task->arg_hst_pointers[i]), 
            task->arg_sizes[i],
            task->arg_types[i],
            is_lit,
            is_from);

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
    return buff;    
}

TargetTaskEntryTy* decode_send_buffer(void * buffer) {
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
    memcpy(&(task->arg_types[0]), cur_ptr, task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 5. loop through arguments and copy values
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
            // set pointer to memory in list
            task->arg_hst_pointers[i] = new_mem;
        }
        
        DBP("decode_send_buffer - arg: " DPxMOD ", size: %ld, type: %ld, literal: %d, from: %d\n", 
            DPxPTR(task->arg_hst_pointers[i]), 
            task->arg_sizes[i],
            task->arg_types[i],
            is_lit,
            is_from);

        cur_ptr += task->arg_sizes[i];
    }

    return task;
}

// should run in a single thread that is always waiting for incoming requests
void* receive_remote_tasks(void* arg) {
    // pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
    // pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pin_thread_to_last_core();
    
    // trigger signal to tell that thread is running now
    pthread_mutex_lock( &_th_receive_remote_tasks_mutex );
    _th_receive_remote_tasks_created = 1; 
    pthread_cond_signal( &_th_receive_remote_tasks_cond );
    pthread_mutex_unlock( &_th_receive_remote_tasks_mutex );

    DBP("receive_remote_tasks (enter)\n");

    int32_t res;
    // intention to reuse buffer over and over again
    int cur_max_buff_size = MAX_BUFFER_SIZE_OFFLOAD_ENTRY;
    void * buffer = malloc(MAX_BUFFER_SIZE_OFFLOAD_ENTRY);
    int recv_buff_size = 0;
    
    while(true) {
        // first check transmission and make sure that buffer has enough memory
        MPI_Status cur_status;
        int flag_open_request = 0;
        
        while(!flag_open_request) {
            usleep(10);
            // check whether thread should be aborted
            if(_flag_abort_threads) {
                DBP("receive_remote_tasks (abort)\n");
                free(buffer);
                int ret_val = 0;
                pthread_exit(&ret_val);
            }
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm, &flag_open_request, &cur_status);
        }

        MPI_Get_count(&cur_status, MPI_BYTE, &recv_buff_size);
        if(recv_buff_size > cur_max_buff_size) {
            // allocate more memory
            free(buffer);
            cur_max_buff_size = recv_buff_size;
            buffer = malloc(recv_buff_size);
        }

        // now receive the data
        res = MPI_Recv(buffer, MAX_BUFFER_SIZE_OFFLOAD_ENTRY, MPI_BYTE, cur_status.MPI_SOURCE, cur_status.MPI_TAG, chameleon_comm, MPI_STATUS_IGNORE);

        // decode task entry
        TargetTaskEntryTy *task = decode_send_buffer(buffer);
        // set information for sending back results/updates if necessary
        task->source_mpi_rank   = cur_status.MPI_SOURCE;
        task->source_mpi_tag    = cur_status.MPI_TAG;

        // add task to stolen list and increment counter
        _mtx_stolen_remote_tasks.lock();
        _stolen_remote_tasks.push_back(task);
        _num_stolen_tasks++;
        trigger_local_load_update();
        _mtx_stolen_remote_tasks.unlock();
    }
}

void* send_back_mapped_data(void *arg) {
    pin_thread_to_last_core();
    
    // trigger signal to tell that thread is running now
    pthread_mutex_lock( &_th_send_back_mapped_data_mutex );
    _th_send_back_mapped_data_created = 1; 
    pthread_cond_signal( &_th_send_back_mapped_data_cond );
    pthread_mutex_unlock( &_th_send_back_mapped_data_mutex );

    int err;
    MPI_Request request_out;
    MPI_Status status_out;
    int request_created = 0;

    DBP("send_back_mapped_data (enter)\n");
    while(true) {
        TargetTaskEntryTy* cur_task = nullptr;
        // exchange work load here before reaching the abort section
        // this is a collective call and needs to be performed at least once

        // avoid overwriting request or keep it up to date?
        // not 100% sure if overwriting a request is a bad idea or makes any problems in MPI
        if(!request_created) {
            _mtx_outstanding_local_jobs.lock();
            int tmp_outstanding = _outstanding_local_jobs;
            _mtx_outstanding_local_jobs.unlock();
            MPI_Iallgather(&tmp_outstanding, 1, MPI_INT, _complete_load_info, 1, MPI_INT, chameleon_comm_load, &request_out);
            request_created = 1;
        }

        int flag_request_avail;
        MPI_Test(&request_out, &flag_request_avail, &status_out);
        if(flag_request_avail) {
            // sum up that stuff
            // DBP("send_back_mapped_data - gathered new load info\n");
            int32_t sum = 0;
            for(int j = 0; j < chameleon_comm_size; j++) {
                // DBP("load info from rank %d = %d\n", j, _complete_load_info[j]);
                sum += _complete_load_info[j];
            }
            _sum_complete_load_info = sum;
            // DBP("complete summed load = %d\n", _sum_complete_load_info);
            // set flag that exchange has happend
            if(!_comm_thread_load_exchange_happend)
                _comm_thread_load_exchange_happend = 1;
            // reset flag
            request_created = 0;
        }

        // check whether to abort thread
        if(_flag_abort_threads) {
            DBP("send_back_mapped_data (abort)\n");
            int ret_val = 0;
            pthread_exit(&ret_val);
        }

        if(_stolen_remote_tasks_send_back.empty()) {
            usleep(10);
            continue;
        }

        _mtx_stolen_remote_tasks_send_back.lock();
        // need to check again
        if(_stolen_remote_tasks_send_back.empty()) {
            continue;
        }
        cur_task = _stolen_remote_tasks_send_back.front();
        _stolen_remote_tasks_send_back.pop_front();
        _mtx_stolen_remote_tasks_send_back.unlock();

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
                memcpy(cur_ptr, cur_task->arg_hst_pointers[i], cur_task->arg_sizes[i]);
                cur_ptr += cur_task->arg_sizes[i];
            }
        }
        // initiate blocking send
        DBP("send_back_mapped_data - sending back data to rank %d with tag %d\n", cur_task->source_mpi_rank, cur_task->source_mpi_tag);
        MPI_Send(buff, tmp_size_buff, MPI_BYTE, cur_task->source_mpi_rank, cur_task->source_mpi_tag, chameleon_comm_mapped);
        free(buff);

        _mtx_stolen_remote_tasks.lock();
        _num_stolen_tasks--;
        trigger_local_load_update();
        _mtx_stolen_remote_tasks.unlock();
    }
}

void trigger_local_load_update() {
    _mtx_outstanding_local_jobs.lock();        
    _outstanding_local_jobs = _num_local_tasks + _num_stolen_tasks;
    DBP("trigger_local_load_update - current oustanding tasks: %d\n", _outstanding_local_jobs);
    _mtx_outstanding_local_jobs.unlock();
}

#ifdef __cplusplus
}
#endif