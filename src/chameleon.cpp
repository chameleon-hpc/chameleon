#include <ffi.h>
#include <mpi.h>
#include <stdexcept>

#include "chameleon.h"
#include "commthread.h"

#ifdef __cplusplus
extern "C" {
#endif

// ================================================================================
// Forward declartion of internal functions (just called inside shared library)
// ================================================================================
int32_t lookup_hst_pointers(TargetTaskEntryTy *task);
int32_t add_offload_entry(TargetTaskEntryTy *task, int rank);
int32_t execute_target_task(TargetTaskEntryTy *task);
int32_t process_remote_task();

int32_t ch_is_initialized = 0;

inline void verify_initialized() {
    if(!ch_is_initialized)
        throw std::runtime_error("Chameleon has not been initilized before.");
}

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

    printf("ChameleonInit from rank %d of %d\n", chameleon_comm_rank, chameleon_comm_size);

    _mtx_local_load_info.lock();
    _local_load_info = 0;
    _mtx_local_load_info.unlock();

    _mtx_complete_load_info.lock();
    _complete_load_info = (int32_t *) malloc(sizeof(int32_t) * chameleon_comm_size);
    for(int i = 0; i < chameleon_comm_size; i++) {
        _complete_load_info[i] = 0;
    }
    _sum_complete_load_info = 0;
    _mtx_complete_load_info.unlock();

    // dummy target region to force binary loading, use host offloading for that purpose
    // #pragma omp target device(1001) // 1001 = CHAMELEON_HOST
    // {
    //     printf("Device Rank %d Dummy: Initializing Chameleon Lib\n", chameleon_comm_rank);
    // }

    // TODO: create communication thread (maybe start later?)

    ch_is_initialized = 1;
    return CHAM_SUCCESS;
}

int32_t chameleon_finalize() {
    verify_initialized();
    return CHAM_SUCCESS;
}

int32_t chameleon_distributed_taskwait() {
    verify_initialized();
    
    // as long as there are local tasks run this loop
    while(true) {
        int32_t res = CHAM_SUCCESS;
        // // ========== Prio 1: try to execute stolen tasks to overlap computation and communication
        // if(!_stolen_remote_tasks.empty()) {
        //     res = process_remote_task();
        //     // if task has been executed successfully start from beginning
        //     if(res == CHAM_REMOTE_TASK_SUCCESS)
        //         continue;
        // }

        // ========== Prio 2: work on local tasks
        if(_local_tasks.empty()) {
            // indicate that all stuff is done
            _mtx_local_tasks.lock();
            _all_local_tasks_done = 1;
            _mtx_local_tasks.unlock();
            break;
        }
        
        TargetTaskEntryTy *cur_task = chameleon_pop_task();
        if(cur_task == nullptr)
        {
            break;
        }
        // if(cur_task) {
        //     // perform lookup in both cases (local execution & offload)
        //     lookup_hst_pointers(cur_task);
        //     // execute region now
        //     res = execute_target_task(cur_task);
        // }

        // DEBUG: force offloading to test MPI communication process (will be done by comm thread later)
        if(cur_task) {
            // perform lookup in both cases (local execution & offload)
            lookup_hst_pointers(cur_task);

            // create temp entry and offload
            OffloadEntryTy *off_entry = new OffloadEntryTy(cur_task, 1);
            res = offload_task_to_rank(off_entry);
        }
    }

    // only execute stolen tasks from here on until all tasks for all ranks are done
    while(true) {
        int32_t res = CHAM_SUCCESS;
        if(!_stolen_remote_tasks.empty()) {
            res = process_remote_task();
            // if task has been executed successfully start from beginning
            if(res == CHAM_REMOTE_TASK_SUCCESS){
                // DEBUG: for testing there will just be one task
                break;
                //continue;
            }
        }

        // TODO: check wether there are still global tasks to be processed
        // TODO: maybe wait for a certain time here
    }

    return CHAM_SUCCESS;
}

int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size) {
    verify_initialized();
    // check list if already in
    _mtx_data_entry.lock();
    int found = 0;
    for(auto &entry : _data_entries) {
        if(entry->tgt_ptr == tgt_ptr && entry->hst_ptr == hst_ptr && entry->size == size) {
            // increase reference count
            entry->ref_count++;
            found = 1;
            break;
        }
    }
    if(!found) {
        // add it to list
        OffloadingDataEntryTy *new_entry = new OffloadingDataEntryTy(tgt_ptr, hst_ptr, size);
        // printf("Creating new Data Entry with address (" DPxMOD ")\n", DPxPTR(&new_entry));
        _data_entries.push_back(new_entry);
    }
    _mtx_data_entry.unlock();
    return CHAM_SUCCESS;
}

int32_t chameleon_add_task(TargetTaskEntryTy *task) {
    verify_initialized();

    _mtx_local_tasks.lock();
    // add to queue
    _local_tasks.push_back(task);
    // increase counter for number of tasks here
    _mtx_local_load_info.lock();
    _local_load_info++;
    _mtx_local_load_info.unlock();

    _all_local_tasks_done = 0;
    _mtx_local_tasks.unlock();

    return CHAM_SUCCESS;
}

TargetTaskEntryTy* chameleon_pop_task() {
    TargetTaskEntryTy* task = nullptr;

    // we do not necessarily need the lock here
    if(_local_tasks.empty())
        return task;

    // get first task in queue
    _mtx_local_tasks.lock();
    if(!_local_tasks.empty()) {
        task = _local_tasks.front();
        _local_tasks.pop_front();

        // decrease local load counter
        _mtx_local_load_info.lock();
        _local_load_info--;
        _mtx_local_load_info.unlock();
    }
    _mtx_local_tasks.unlock();
    return task;
}

int32_t lookup_hst_pointers(TargetTaskEntryTy *task) {
    for(int i = 0; i < task->arg_num; i++) {
        // get type and pointer
        int64_t tmp_type    = task->arg_types[i];
        void * tmp_tgt_ptr  = task->arg_tgt_converted_pointers[i];

        if(tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL) {
            // pointer represents numerical value that is implicitly mapped
            task->arg_hst_pointers[i] = tmp_tgt_ptr;
            task->arg_sizes[i] = sizeof(void *);
        } else {
            // here we need to perform a pointer mapping to host pointer
            // because target pointers have already been freed and deleted
            int found = 0;
            for(auto &entry : _data_entries) {
                // printf("Checking Mapping Entry (" DPxMOD ")\n", DPxPTR(entry.tgt_ptr));
                if(entry->tgt_ptr == tmp_tgt_ptr) {
                    task->arg_sizes[i] = entry->size;
                    task->arg_hst_pointers[i] = entry->hst_ptr;
                    found = 1;
                    break;
                }
            }
            if(!found) {
                // something went wrong here
                printf("Error: No mapping entry found for address (" DPxMOD ")\n", DPxPTR(tmp_tgt_ptr));
                return CHAM_FAILURE;
            }
        }
    }
    return CHAM_SUCCESS;
}

int32_t add_offload_entry(TargetTaskEntryTy *task, int rank) {
    // create new entry
    OffloadEntryTy *new_entry = new OffloadEntryTy(task, rank);

    // save in list
    _mtx_offload_entries.lock();
    _offload_entries.push_back(new_entry);
    _mtx_offload_entries.unlock();

    return CHAM_SUCCESS;
}

int32_t execute_target_task(TargetTaskEntryTy *task) {
    // Use libffi to launch execution.
    ffi_cif cif;

    // All args are references.
    std::vector<ffi_type *> args_types(task->arg_num, &ffi_type_pointer);

    std::vector<void *> args(task->arg_num);
    std::vector<void *> ptrs(task->arg_num);

    for (int32_t i = 0; i < task->arg_num; ++i) {
        int64_t tmp_type        = task->arg_types[i];
        int64_t is_literal      = (tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL);
        int64_t is_implicit     = (tmp_type & CHAM_OMP_TGT_MAPTYPE_IMPLICIT);
        int64_t is_to           = (tmp_type & CHAM_OMP_TGT_MAPTYPE_TO);
        int64_t is_from         = (tmp_type & CHAM_OMP_TGT_MAPTYPE_FROM);
        int64_t is_prt_obj      = (tmp_type & CHAM_OMP_TGT_MAPTYPE_PTR_AND_OBJ);

        ptrs[i] = task->arg_tgt_converted_pointers[i];

        if(tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL) {
            // no need to do anything because it is by value
            args[i] = &ptrs[i];
            continue;
        } else {
            ptrs[i] = task->arg_hst_pointers[i];
            args[i] = &ptrs[i];
        }
    }
    
    ffi_status status = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, task->arg_num, &ffi_type_void, &args_types[0]);

    if(status != FFI_OK) {
        printf("Unable to prepare target launch!\n");
        return CHAM_FAILURE;
    }

    void (*entry)(void);
    *((void**) &entry) = ((void*) task->tgt_entry_ptr);
    // use host pointers here
    ffi_call(&cif, entry, NULL, &args[0]);

    return CHAM_SUCCESS;
}

inline int32_t process_remote_task() {
    TargetTaskEntryTy *remote_task = nullptr;
    
    _mtx_stolen_remote_tasks.lock();
    std::list<TargetTaskEntryTy*>::iterator it;
    for (it = _stolen_remote_tasks.begin(); it != _stolen_remote_tasks.end(); ++it) {
        remote_task = *it;
        if(remote_task->status == CHAM_TASK_STATUS_OPEN) {
            // mark task to be processed
            remote_task->status = CHAM_TASK_STATUS_PROCESSING;
            break;
        }
        remote_task = nullptr;
    }
    _mtx_stolen_remote_tasks.unlock();

    // currently no free task to be processed
    if(!remote_task)
        return CHAM_REMOTE_TASK_NONE;

    // execute region now
    int32_t res = execute_target_task(remote_task);
    if(res != CHAM_SUCCESS)
        return res;
            
    // mark as done; communication thread can now send data back to sender if there is any
    _mtx_stolen_remote_tasks.lock();
    remote_task->status = CHAM_TASK_STATUS_DONE;
    _mtx_stolen_remote_tasks.unlock();
    return CHAM_REMOTE_TASK_SUCCESS;
}

#ifdef __cplusplus
}
#endif
