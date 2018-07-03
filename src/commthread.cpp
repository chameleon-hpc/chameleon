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
    // number of arguments = int64_t (always keep 8 byte)
    // array with argument types = n_args * int64_t
    // array with length of argument pointers = n_args * int64_t
    // array with values

    // how about MPI packed data types?

    // int total_size = 
    //     sizeof(intptr_t) + 
    //     sizeof(int64_t) + 
    //     task.arg_num*sizeof(int64_t) + 
    //     task.arg_num*sizeof(int64_t);

    // std::vector<void *> arg_src_pointers(task.arg_num);
    // std::vector<void *> arg_tgt_pointers(task.arg_num);
    // std::vector<int64_t> arg_sizes(task.arg_num);
    // std::vector<int64_t> arg_types(task.arg_num);

    // for(int i = 0; i < task.arg_num; i++) {
    //     // determine source pointer 
    //     arg_types[i] = task.tgt_arg_types[i];
    //     arg_tgt_pointers[i] = (void *)((intptr_t)task.tgt_args[i] + task.tgt_offsets[i]);

    //     if(arg_types[i] & CH_OMP_TGT_MAPTYPE_LITERAL) {
    //         // pointer represents numerical value that is implicitly mapped
    //         arg_sizes[i] = sizeof(void *);
    //         arg_src_pointers[i] = arg_tgt_pointers[i];
    //     } else {            
    //         // here we need to perform a pointer mapping to source pointers 
    //         // because target pointers have already been deleted
    //         int found = 0;
    //         for(auto &entry : _data_entries) {
    //             // printf("Checking Mapping Entry (" DPxMOD ")\n", DPxPTR(entry.tgt_ptr));
    //             if(entry.tgt_ptr == arg_tgt_pointers[i]) {
    //                 arg_sizes[i] = entry.size;
    //                 arg_src_pointers[i] = entry.hst_ptr;
    //                 found = 1;
    //                 break;
    //             }
    //         }
    //         if(!found) {
    //             // something went wrong here
    //             printf("Error: No mapping entry found for address (" DPxMOD ")\n", DPxPTR(arg_tgt_pointers[i]));
    //         }
    //     }
    //     // append to total size
    //     total_size += arg_sizes[i];        
    // }

    // // allocate memory
    // void * input_buff = malloc(total_size);
    // ((intptr_t *) input_buff)[0] = (intptr_t) task.tgt_entry_ptr;
    // void * tmp_ptr = (void *)((intptr_t *) input_buff)[0];

    return 0;
}

#ifdef __cplusplus
}
#endif