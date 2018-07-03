#include "commthread.h"

MPI_Comm chameleon_comm;
int chameleon_comm_rank;
int chameleon_comm_size;

std::mutex _mtx_data_entry;
std::list<OffloadingDataEntryTy> _data_entries;
std::mutex _mtx_tasks;
std::list<OffloadingTaskEntryTy> _tasks;

#ifdef __cplusplus
extern "C" {
#endif

int32_t offload_task_to_rank(OffloadingTaskEntryTy task, int rank) {
    // TODO: use non blocking communication for that purpose
    // TODO: How does the format look like?
    
    // target function pointer = address
    // number of arguments = int32_t
    // array with argument types = n_args * int64_t
    // array with length of argument pointers = n_args * int64_t
    // array with values

    // how about MPI packed data types?

    int total_size = sizeof(intptr_t)       // 1. target entry pointer
        + sizeof(int32_t)                   // 2. number of arguments
        + task.arg_num*sizeof(int64_t)      // 3. argument sizes
        + task.arg_num*sizeof(int64_t);     // 4. argument types

    std::vector<void *> arg_hst_pointers(task.arg_num);
    std::vector<void *> arg_tgt_pointers(task.arg_num);
    std::vector<int64_t> arg_sizes(task.arg_num);
    std::vector<int64_t> arg_types(task.arg_num);

    for(int i = 0; i < task.arg_num; i++) {
        // get type and pointer
        arg_types[i] = task.tgt_arg_types[i];
        arg_tgt_pointers[i] = (void *)((intptr_t)task.tgt_args[i] + task.tgt_offsets[i]);

        if(arg_types[i] & CH_OMP_TGT_MAPTYPE_LITERAL) {
            // pointer represents numerical value that is implicitly mapped
            arg_sizes[i] = sizeof(void *);
            arg_hst_pointers[i] = arg_tgt_pointers[i];
        } else {            
            // here we need to perform a pointer mapping to host pointer
            // because target pointers have already been freed and deleted
            int found = 0;
            for(auto &entry : _data_entries) {
                // printf("Checking Mapping Entry (" DPxMOD ")\n", DPxPTR(entry.tgt_ptr));
                if(entry.tgt_ptr == arg_tgt_pointers[i]) {
                    arg_sizes[i] = entry.size;
                    arg_hst_pointers[i] = entry.hst_ptr;
                    found = 1;
                    break;
                }
            }
            if(!found) {
                // something went wrong here
                printf("Error: No mapping entry found for address (" DPxMOD ")\n", DPxPTR(arg_tgt_pointers[i]));
            }
        }
        // append to total size
        total_size += arg_sizes[i];        
    }

    // allocate memory for transfer
    char *input_buff = (char *) malloc(total_size);
    char *cur_ptr = (char *)input_buff;
    // 1. target entry address
    ((intptr_t *) cur_ptr)[0] = (intptr_t) task.tgt_entry_ptr;
    cur_ptr += sizeof(intptr_t);
    // 2. number of arguments
    ((int32_t *) cur_ptr)[0] = task.arg_num;
    cur_ptr += sizeof(int32_t);
    // 3. argument sizes
    memcpy(cur_ptr, &arg_sizes[0], task.arg_num*sizeof(int64_t));
    cur_ptr += task.arg_num*sizeof(int64_t);
    // 4. argument types
    memcpy(cur_ptr, &arg_types[0], task.arg_num*sizeof(int64_t));
    cur_ptr += task.arg_num*sizeof(int64_t);
    // 5. loop through arguments and copy values
    for(int32_t i = 0; i < task.arg_num; i++) {
        // copy value from host pointer directly
        if(arg_types[i] & CH_OMP_TGT_MAPTYPE_LITERAL) {
            ((intptr_t *) cur_ptr)[0] = (intptr_t) arg_hst_pointers[i];
        } else {
            memcpy(cur_ptr, (char*)arg_hst_pointers[i], arg_sizes[i]);
        }
        cur_ptr += arg_sizes[i];
    }

    // TODO: send data to target rank

    // TODO: temp buffers to retreive output data from target rank to be able to update host pointers

    return 0;
}

#ifdef __cplusplus
}
#endif