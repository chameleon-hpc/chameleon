#include "commthread.h"

MPI_Comm chameleon_comm;
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

#ifdef __cplusplus
extern "C" {
#endif
// ================================================================================
// Forward declartion of internal functions (just called inside shared library)
// ================================================================================
void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size);
TargetTaskEntryTy* decode_send_buffer(void * buffer);

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
    DBP("offload_task_to_rank - sending data to target rank with tag: %d\n", tmp_tag);
    MPI_Request request = MPI_REQUEST_NULL;
    MPI_Status status;
    MPI_Isend(buffer, buffer_size, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm, &request);
    
    // wait until communication is finished
    MPI_Wait(&request, &status);
    free(buffer);

    if(has_outputs) {
        // temp buffer to retreive output data from target rank to be able to update host pointers again
        void * temp_buffer = malloc(MAX_BUFFER_SIZE_OFFLOAD_ENTRY);
        MPI_Recv(temp_buffer, MAX_BUFFER_SIZE_OFFLOAD_ENTRY, MPI_BYTE, entry->target_rank, tmp_tag, chameleon_comm, MPI_STATUS_IGNORE);

        DBP("offload_task_to_rank - receiving output data for tag: %d\n", tmp_tag);
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
int32_t receive_remote_tasks() {
    DBP("receive_remote_tasks (enter)\n");
    int32_t res;
    void * buffer = malloc(MAX_BUFFER_SIZE_OFFLOAD_ENTRY);

    // while(true) {
    MPI_Status cur_status;
    res = MPI_Recv(buffer, MAX_BUFFER_SIZE_OFFLOAD_ENTRY, MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, chameleon_comm, &cur_status);

    // decode task entry
    TargetTaskEntryTy *task = decode_send_buffer(buffer);
    // free buffer again
    free(buffer);
    // set information for sending back results/updates if necessary
    task->source_mpi_rank   = cur_status.MPI_SOURCE;
    task->source_mpi_tag    = cur_status.MPI_TAG;

    // add task to stolen list
    _mtx_stolen_remote_tasks.lock();
    _stolen_remote_tasks.push_back(task);
    _mtx_stolen_remote_tasks.unlock();
    // }

    return CHAM_SUCCESS;
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
        MPI_Send(buff, tmp_size_buff, MPI_BYTE, cur_task->source_mpi_rank, cur_task->source_mpi_tag, chameleon_comm);
    }
    // }
    return CHAM_SUCCESS;
}

#ifdef __cplusplus
}
#endif