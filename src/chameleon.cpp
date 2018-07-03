#include <ffi.h>
#include <mpi.h>

#include "chameleon.h"
#include "commthread.h"

// MPI_Comm chameleon_comm;
// int chameleon_comm_rank;
// int chameleon_comm_size;

// std::mutex _mtx_data_entry;
// std::list<OffloadingDataEntryTy> _data_entries;
// std::mutex _mtx_tasks;
// std::list<OffloadingTaskEntryTy> _tasks;

#ifdef __cplusplus
extern "C" {
#endif

int32_t chameleon_init() {
    // check whether MPI is initialized, otherwise do so
    int initialized, err;
    initialized = 0;
    err = MPI_Initialized(&initialized);
    if(!initialized) {
        MPI_Init(NULL, NULL);
    }

    // create separate communicator for chameleon
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm);
    MPI_Comm_size(chameleon_comm, &chameleon_comm_size);
    MPI_Comm_rank(chameleon_comm, &chameleon_comm_rank);

    printf("Chameleon: Hello from rank %d of %d\n", chameleon_comm_rank, chameleon_comm_size);

    // dummy target region to force binary loading, use host offloading for that purpose
    #pragma omp target device(1001) // 1001 = CHAMELEON_HOST
    {
        printf("Device Rank %d Dummy: Initializing Chameleon Lib\n", chameleon_comm_rank);
    }

    // TODO: create communication thread (maybe start later?)

    return CHAM_SUCCESS;
}

int32_t chameleon_finalize() {
    return CHAM_SUCCESS;
}

int32_t chameleon_distributed_taskwait() {
    // first shot is to execute the tasks in place
    while(true) {
        // DEBUG info
        bool test1 = _tasks.empty();
        size_t test2 = _tasks.size();

        if(_tasks.empty())
            break;
        
        // get first task
        OffloadingTaskEntryTy cur_task = _tasks.front();
        _tasks.pop_front();

        // Use libffi to launch execution.
        ffi_cif cif;

        // All args are references.
        std::vector<ffi_type *> args_types(cur_task.arg_num, &ffi_type_pointer);
        std::vector<void *> args(cur_task.arg_num);
        std::vector<void *> ptrs(cur_task.arg_num);

        //printf("New OffloadingTaskEntryTy\n");
        for (int32_t i = 0; i < cur_task.arg_num; ++i) {
            printf("- adding parameter address (" DPxMOD ") with offset = %td\n", DPxPTR(cur_task.tgt_args[i]), cur_task.tgt_offsets[i]);

            int64_t tmp_type        = cur_task.tgt_arg_types[i];
            int64_t is_literal      = (tmp_type & CH_OMP_TGT_MAPTYPE_LITERAL);
            int64_t is_implicit     = (tmp_type & CH_OMP_TGT_MAPTYPE_IMPLICIT);
            int64_t is_to           = (tmp_type & CH_OMP_TGT_MAPTYPE_TO);
            int64_t is_from         = (tmp_type & CH_OMP_TGT_MAPTYPE_FROM);
            int64_t is_prt_obj      = (tmp_type & CH_OMP_TGT_MAPTYPE_PTR_AND_OBJ);

            ptrs[i] = (void *)((intptr_t)cur_task.tgt_args[i] + cur_task.tgt_offsets[i]);

            if(cur_task.tgt_arg_types[i] & CH_OMP_TGT_MAPTYPE_LITERAL) {
                // no need to do anything because it is by value
                args[i] = &ptrs[i];
                continue;
            }

            // here we need to perform a pointer mapping to source pointers 
            // because target pointers have already been deleted
            int found = 0;
            void *tmp_ptr = ptrs[i];
            for(auto &entry : _data_entries) {
                printf("Checking Mapping Entry (" DPxMOD ")\n", DPxPTR(entry.tgt_ptr));
                if(entry.tgt_ptr == tmp_ptr) {
                    // increase reference count
                    ptrs[i] = entry.hst_ptr;
                    found = 1;
                    break;
                }
            }
            if(!found) {
                // something went wrong here
                printf("Error: No mapping entry found for address (" DPxMOD ")\n", DPxPTR(ptrs[i]));
                //throw std::runtime_error("Error: Could not find host pointer entry for target pointer...");
            }
            args[i] = &ptrs[i];
        }

        ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, cur_task.arg_num,
                                        &ffi_type_void, &args_types[0]);

        if(status != FFI_OK) {
            //assert(status == FFI_OK && "Unable to prepare target launch!");
            return CHAM_FAILURE;
        }

        void (*entry)(void);
        *((void**) &entry) = cur_task.tgt_entry_ptr;
        ffi_call(&cif, entry, NULL, &args[0]);
    }

    // TODO: Send around information that this rank does not have any tasks left

    // Try to get incoming requests from other ranks

    return CHAM_SUCCESS;
}

int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size) {
    // check list if already in
    _mtx_data_entry.lock();
    int found = 0;
    for(auto &entry : _data_entries) {
        if(entry.tgt_ptr == tgt_ptr && entry.hst_ptr == hst_ptr && entry.size == size) {
            // increase reference count
            entry.ref_count++;
            found = 1;
            break;
        }
    }
    if(!found) {
        // add it to list
        OffloadingDataEntryTy new_entry(tgt_ptr, hst_ptr, size);
        // printf("Creating new Data Entry with address (" DPxMOD ")\n", DPxPTR(&new_entry));
        _data_entries.push_back(new_entry);
    }
    _mtx_data_entry.unlock();
    return CHAM_SUCCESS;
}

int32_t chameleon_add_task(OffloadingTaskEntryTy task) {
    _mtx_tasks.lock();
    _tasks.push_back(task);

    bool test1 = _tasks.empty();
    size_t test2 = _tasks.size();
    _mtx_tasks.unlock();
    return CHAM_SUCCESS;
}

#ifdef __cplusplus
}
#endif
