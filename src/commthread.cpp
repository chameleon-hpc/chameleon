#include "commthread.h"

MPI_Comm chameleon_comm;
int chameleon_comm_rank;
int chameleon_comm_size;

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

int32_t offload_task_to_rank(OffloadEntryTy *entry) {
    // encode buffer
    int32_t buffer_size = 0;
    void *buffer = encode_send_buffer(entry->task_entry, &buffer_size);

    // TODO: send data to target rank
    // MPI_Isend()    

    // TODO: temp buffers to retreive output data from target rank to be able to update host pointers again

    return CHAM_SUCCESS;
}

void * encode_send_buffer(TargetTaskEntryTy *task, int32_t *buffer_size) {

    // FORMAT:
    //      1. target function pointer = address (intptr_t)
    //      2. number of arguments = int32_t
    //      3. array with argument types = n_args * int64_t
    //      4. array with length of argument pointers = n_args * int64_t
    //      5. array with values

    // TODO: Is it worth while to consider MPI packed data types??

    int total_size = sizeof(intptr_t)           // 1. target entry pointer
        + sizeof(int32_t)                       // 2. number of arguments
        + task->arg_num * sizeof(int64_t)       // 3. argument sizes
        + task->arg_num * sizeof(int64_t);      // 4. argument types

    for(int i = 0; i < task->arg_num; i++) {
        total_size += task->arg_sizes[i];
    }

    // allocate memory for transfer
    char *buff = (char *) malloc(total_size);
    char *cur_ptr = (char *)buff;

    // 1. target entry address
    ((intptr_t *) cur_ptr)[0] = (intptr_t) task->tgt_entry_ptr;
    cur_ptr += sizeof(intptr_t);

    // 2. number of arguments
    ((int32_t *) cur_ptr)[0] = task->arg_num;
    cur_ptr += sizeof(int32_t);

    // 3. argument sizes
    memcpy(cur_ptr, &(task->arg_sizes[0]), task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 4. argument types
    memcpy(cur_ptr, &(task->arg_types[0]), task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 5. loop through arguments and copy values
    for(int32_t i = 0; i < task->arg_num; i++) {
        // copy value from host pointer directly
        if(task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL) {
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
    // 2. number of arguments
    task->arg_num = ((int32_t *) cur_ptr)[0];
    cur_ptr += sizeof(int32_t);

    // resize data structure
    task->ReSizeArrays(task->arg_num);
    // task->arg_sizes.resize(task->arg_num);
    // task->arg_types.resize(task->arg_num);
    // task->arg_types.resize(task->arg_hst_pointers);
    
    // 3. argument sizes
    memcpy(&(task->arg_sizes[0]), cur_ptr, task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 4. argument types
    memcpy(&(task->arg_types[0]), cur_ptr, task->arg_num * sizeof(int64_t));
    cur_ptr += task->arg_num * sizeof(int64_t);

    // 5. loop through arguments and copy values
    for(int32_t i = 0; i < task->arg_num; i++) {
        // copy value from host pointer directly
        if(task->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_LITERAL) {
            ((intptr_t*) task->arg_hst_pointers[i])[0] = ((intptr_t *) cur_ptr)[0];
        } else {
            // need to allocate new memory
            void * new_mem = malloc(task->arg_sizes[i]);
            memcpy(new_mem, cur_ptr, task->arg_sizes[i]);
            // set pointer to memory in list
            task->arg_hst_pointers[i] = new_mem;
        }
        cur_ptr += task->arg_sizes[i];
    }

    return task;
}

#ifdef __cplusplus
}
#endif