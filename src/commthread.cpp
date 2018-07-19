#include "commthread.h"
#include <pthread.h>
#include <signal.h>

#include <sched.h>
#include <hwloc.h>

MPI_Comm chameleon_comm;
MPI_Comm chameleon_comm_mapped;
int chameleon_comm_rank;
int chameleon_comm_size;

std::vector<intptr_t> _image_base_addresses;

// list with data that has been mapped in map clauses
std::mutex _mtx_data_entry;
std::list<OffloadingDataEntryTy*> _data_entries;

// list with local task entries
// these can either be executed here or offloaded to a different rank
std::mutex _mtx_local_tasks;
std::list<TargetTaskEntryTy*> _local_tasks;

int32_t _all_local_tasks_done = 0;

// list with stolen task entries that should be executed
std::mutex _mtx_stolen_remote_tasks;
std::list<TargetTaskEntryTy*> _stolen_remote_tasks;

// entries that should be offloaded to specific ranks
std::mutex _mtx_offload_entries;
std::list<OffloadEntryTy*> _offload_entries;

// Load Information (temporary here: number of tasks)
std::mutex _mtx_local_load_info;
int32_t _local_load_info;

std::mutex _mtx_complete_load_info;
int32_t *_complete_load_info;
int32_t _sum_complete_load_info;

const int32_t MAX_BUFFER_SIZE_OFFLOAD_ENTRY = 20480; // 20 KB for testing

// ============== Thread Section ===========
pthread_t           th_receive_remote_tasks;
int                 th_receive_remote_tasks_created = 0;
pthread_cond_t      th_receive_remote_tasks_cond    = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     th_receive_remote_tasks_mutex   = PTHREAD_MUTEX_INITIALIZER;

#ifdef __cplusplus
extern "C" {
#endif
// ================================================================================
// Forward declartion of internal functions (just called inside shared library)
// ================================================================================
void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size);
TargetTaskEntryTy* decode_send_buffer(void * buffer);
short pin_thread_to_last_core();

#pragma region Start/Stop/Pin Communication Threads
int32_t start_communication_threads() {
    DBP("start_communication_threads (enter)\n");
    int err;
    err = pthread_create(&th_receive_remote_tasks, NULL, receive_remote_tasks, NULL);
    if(err != 0)
        handle_error_en(err, "pthread_create - th_receive_remote_tasks");
    
    // wait for finished thread creation??? Is that a good idea?
    pthread_mutex_lock(&th_receive_remote_tasks_mutex);
    while (th_receive_remote_tasks_created == 0) {
        pthread_cond_wait(&th_receive_remote_tasks_cond, &th_receive_remote_tasks_mutex);
    }
    pthread_mutex_unlock(&th_receive_remote_tasks_mutex);

    return CHAM_SUCCESS;
}

int32_t stop_communication_threads() {
    DBP("stop_communication_threads (enter)\n");
    int err;
    // first kill all threads
    err = pthread_cancel(th_receive_remote_tasks);
    // err = pthread_kill(th_receive_remote_tasks, SIGKILL);
    // then wait for all threads to finish
    err = pthread_join(th_receive_remote_tasks, NULL);
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

// This should run in a single pthread because it should be blocking
int32_t offload_task_to_rank(OffloadEntryTy *entry) {
    int has_outputs = entry->task_entry->HasAtLeastOneOutput();
    DBP("offload_task_to_rank (enter) - task_entry: " DPxMOD ", num_args: %d, rank: %d, has_output: %d\n", DPxPTR(entry->task_entry->tgt_entry_ptr), entry->task_entry->arg_num, entry->target_rank, has_outputs);

    // encode buffer
    int32_t buffer_size = 0;
    void * buffer = encode_send_buffer(entry->task_entry, &buffer_size);

    // TODO: calculate proper tag that contains bit combination of sender as well as 
    // "task id" (maybe global increment per process)
    int tmp_tag = 0;
    
    // send data to target rank
    DBP("offload_task_to_rank - sending data to target rank %d with tag: %d\n", entry->target_rank, tmp_tag);
    // MPI_Request request = MPI_REQUEST_NULL;
    // MPI_Status status;
    // MPI_Isend(buffer, buffer_size, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm, &request);
    // // wait until communication is finished
    // MPI_Wait(&request, &status);
    MPI_Send(buffer, buffer_size, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm);
    free(buffer);

    if(has_outputs) {
        DBP("offload_task_to_rank - waiting for output data from rank %d for tag: %d\n", entry->target_rank, tmp_tag);
        // temp buffer to retreive output data from target rank to be able to update host pointers again
        void * temp_buffer = malloc(MAX_BUFFER_SIZE_OFFLOAD_ENTRY);
        MPI_Status cur_status;
        MPI_Recv(temp_buffer, MAX_BUFFER_SIZE_OFFLOAD_ENTRY, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm_mapped, &cur_status);

        DBP("offload_task_to_rank - receiving output data from rank %d for tag: %d\n", cur_status.MPI_SOURCE, cur_status.MPI_TAG);
        // copy results back to source pointers with memcpy
        char * cur_ptr = (char*) temp_buffer;
        for(int i = 0; i < entry->task_entry->arg_num; i++) {
            int is_lit      = entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL;
            int is_from     = entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM;

            if(entry->task_entry->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM) {
                DBP("offload_task_to_rank - arg: " DPxMOD ", size: %ld, type: %ld, literal: %d, from: %d\n", 
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

    return CHAM_SUCCESS;
}

void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size) {
    DBP("encode_send_buffer (enter) - task_entry: " DPxMOD "(idx:%d;offset:%d), num_args: %d\n", DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);

    // FORMAT:
    //      1. target function pointer = address (intptr_t)
    //      2. number of arguments = int32_t
    //      3. array with argument types = n_args * int64_t
    //      4. array with length of argument pointers = n_args * int64_t
    //      5. array with values

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

    // 5. loop through arguments and copy values
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
    pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pin_thread_to_last_core();
    
    // trigger signal to tell that thread is running now
    pthread_mutex_lock( &th_receive_remote_tasks_mutex );
    th_receive_remote_tasks_created = 1; 
    pthread_cond_signal( &th_receive_remote_tasks_cond );
    pthread_mutex_unlock( &th_receive_remote_tasks_mutex );

    DBP("receive_remote_tasks (enter)\n");

    int32_t res;
    // intention to reuse buffer over and over again
    int cur_max_buff_size = MAX_BUFFER_SIZE_OFFLOAD_ENTRY;
    void * buffer = malloc(MAX_BUFFER_SIZE_OFFLOAD_ENTRY);
    int recv_buff_size = 0;
    
    while(true) {
        // first check transmission and make sure that buffer has enough memory
        MPI_Status cur_status;
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm, &cur_status);
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

        // add task to stolen list
        _mtx_stolen_remote_tasks.lock();
        _stolen_remote_tasks.push_back(task);
        _mtx_stolen_remote_tasks.unlock();
    }

    // free buffer again; currently unreachable code since thread will be canceled
    // free(buffer);
    return nullptr;
}

int32_t send_back_remote_task_data() {
    DBP("send_back_remote_task_data (enter)\n");
    // while(true) {
    TargetTaskEntryTy* cur_task = nullptr;
    _mtx_stolen_remote_tasks.lock();
    std::list<TargetTaskEntryTy*>::iterator it;
    for (it = _stolen_remote_tasks.begin(); it != _stolen_remote_tasks.end(); ++it) {
        if((*it)->status == CHAM_TASK_STATUS_DONE) {
            // get and remove task here
            cur_task = *it;
            _stolen_remote_tasks.erase(it++);
            break;
        }
    }
    _mtx_stolen_remote_tasks.unlock();

    // only send data if there is at least one output, otherwise we can skip transfer
    if(cur_task && cur_task->HasAtLeastOneOutput()) {
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
        DBP("send_back_remote_task_data - sending back data to rank %d with tag %d\n", cur_task->source_mpi_rank, cur_task->source_mpi_tag);
        MPI_Send(buff, tmp_size_buff, MPI_BYTE, cur_task->source_mpi_rank, cur_task->source_mpi_tag, chameleon_comm_mapped);
        // free again
        free(buff);
    }
    // }
    return CHAM_SUCCESS;
}

#ifdef __cplusplus
}
#endif