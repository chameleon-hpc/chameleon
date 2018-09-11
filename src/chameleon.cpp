#include <ffi.h>
#include <mpi.h>
#include <stdexcept>

#include "chameleon.h"
#include "commthread.h"
#include "cham_statistics.h"

#ifndef FORCE_OFFLOAD_MASTER_WORKER
#define FORCE_OFFLOAD_MASTER_WORKER 0
#endif

#ifdef TRACE
#include "VT.h"
#endif

int TargetTaskEntryTy::HasAtLeastOneOutput() {
    for(int i = 0; i < this->arg_num; i++) {
        if(this->arg_types[i] & CHAM_OMP_TGT_MAPTYPE_FROM)
            return 1;
    }
    return 0;
}

#ifdef __cplusplus
extern "C" {
#endif

// ================================================================================
// Forward declartion of internal functions (just called inside shared library)
// ================================================================================
int32_t lookup_hst_pointers(TargetTaskEntryTy *task);
// int32_t add_offload_entry(TargetTaskEntryTy *task, int rank);
int32_t execute_target_task(TargetTaskEntryTy *task);
int32_t process_remote_task();

std::mutex _mtx_ch_is_initialized;
int32_t _ch_is_initialized = 0;

inline void verify_initialized() {
    if(!_ch_is_initialized)
        throw std::runtime_error("Chameleon has not been initilized before.");
}

int32_t chameleon_init() {
    if(_ch_is_initialized)
        return CHAM_SUCCESS;
    
    _mtx_ch_is_initialized.lock();
    // need to check again
    if(_ch_is_initialized) {
        _mtx_ch_is_initialized.unlock();
        return CHAM_SUCCESS;
    }

    // check whether MPI is initialized, otherwise do so
    int initialized, err;
    initialized = 0;
    err = MPI_Initialized(&initialized);
    if(!initialized) {
        // MPI_Init(NULL, NULL);
        int provided;
        MPI_Init_thread(NULL, NULL, MPI_THREAD_MULTIPLE, &provided);
    }

    // create separate communicator for chameleon
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm);
    if(err != 0) handle_error_en(err, "MPI_Comm_dup - chameleon_comm");
    MPI_Comm_size(chameleon_comm, &chameleon_comm_size);
    MPI_Comm_rank(chameleon_comm, &chameleon_comm_rank);
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm_mapped);
    if(err != 0) handle_error_en(err, "MPI_Comm_dup - chameleon_comm_mapped");
    err = MPI_Comm_dup(MPI_COMM_WORLD, &chameleon_comm_load);
    if(err != 0) handle_error_en(err, "MPI_Comm_dup - chameleon_comm_load");

    DBP("chameleon_init\n");

    _mtx_load_exchange.lock();
    _outstanding_jobs_ranks.resize(chameleon_comm_size);
    _load_info_ranks.resize(chameleon_comm_size);
    for(int i = 0; i < chameleon_comm_size; i++) {
        _outstanding_jobs_ranks[i] = 0;
        _load_info_ranks[i] = 0;
    }
    _outstanding_jobs_sum = 0;
    _load_info_sum = 0;
    _mtx_load_exchange.unlock();

    // dummy target region to force binary loading, use host offloading for that purpose
    #pragma omp target device(1001) map(to:stderr) // 1001 = CHAMELEON_HOST
    {
        DBP("chameleon_init - dummy region\n");
    }
        
    // set flag to ensure that only a single thread is initializing
    _ch_is_initialized = 1;
    _mtx_ch_is_initialized.unlock();
    return CHAM_SUCCESS;
}

int32_t chameleon_set_image_base_address(int idx_image, intptr_t base_address) {
    if(_image_base_addresses.size() < idx_image+1) {
        _image_base_addresses.resize(idx_image+1);
    }
    // set base address for image (last device wins)
    DBP("chameleon_set_image_base_address (enter) Setting base_address: " DPxMOD " for img: %d\n", DPxPTR((void*)base_address), idx_image);
    _image_base_addresses[idx_image] = base_address;
    return CHAM_SUCCESS;
}

int32_t chameleon_finalize() {
    DBP("chameleon_finalize (enter)\n");
    verify_initialized();
    DBP("chameleon_finalize (exit)\n");
    return CHAM_SUCCESS;
}

#if !FORCE_OFFLOAD_MASTER_WORKER
int32_t chameleon_distributed_taskwait(int nowait) {
#ifdef TRACE
    static int event_process_remote = -1;
    static const std::string event_process_remote_name = "process_remote";
    if( event_process_remote == -1) 
        int ierr = VT_funcdef(event_process_remote_name.c_str(), VT_NOCLASS, &event_process_remote);
    static int event_process_local = -1;
    static const std::string event_process_local_name = "process_local";
    if( event_process_local == -1)
        int ierr = VT_funcdef(event_process_local_name.c_str(), VT_NOCLASS, &event_process_local);
#endif
    DBP("chameleon_distributed_taskwait (enter)\n");
    verify_initialized();

    // start communication threads here
    start_communication_threads();
    
    // as long as there are local tasks run this loop
    while(true) {
        int32_t res = CHAM_SUCCESS;
        // ========== Prio 1: try to execute stolen tasks to overlap computation and communication
        if(!_stolen_remote_tasks.empty()) {
#ifdef TRACE
            VT_begin(event_process_remote);
#endif
            res = process_remote_task();
#ifdef TRACE
            VT_end(event_process_remote);
#endif
            // if task has been executed successfully start from beginning
            if(res == CHAM_REMOTE_TASK_SUCCESS)
                continue;
        }

        // ========== Prio 2: check whether to abort procedure
        //only abort if load exchange has happened at least once and there are no outstanding jobs left
        if(_comm_thread_load_exchange_happend && _outstanding_jobs_sum == 0) {
            break;
        }

        // ========== Prio 3: work on local tasks
        if(!_local_tasks.empty()) {
            TargetTaskEntryTy *cur_task = chameleon_pop_task();
            if(cur_task == nullptr)
            {
                continue;
            }
            
            if(cur_task) {
                // execute region now
                DBP("chameleon_distributed_taskwait - local task execution\n");

#if CHAM_STATS_RECORD
                double cur_time = omp_get_wtime();
#endif
#ifdef TRACE
                VT_begin(event_process_local);
#endif
                res = execute_target_task(cur_task);
#ifdef TRACE
                VT_end(event_process_local);
#endif
#if CHAM_STATS_RECORD
                cur_time = omp_get_wtime()-cur_time;
                _mtx_time_task_execution_local.lock();
                _time_task_execution_local_sum += cur_time;
                _time_task_execution_local_count++;
                _mtx_time_task_execution_local.unlock();
#endif
                // it is save to decrement counter after local execution
                _mtx_load_exchange.lock();
                _num_local_tasks_outstanding--;
                _load_info_local--;
                trigger_update_outstanding();
                _mtx_load_exchange.unlock();

#if OFFLOAD_BLOCKING
                _mtx_offload_blocked.lock();
                _offload_blocked = 0;
                _mtx_offload_blocked.unlock();
#endif

#if CHAM_STATS_RECORD
                _mtx_num_executed_tasks_local.lock();
                _num_executed_tasks_local++;
                _mtx_num_executed_tasks_local.unlock();
#endif
            }
        }
    }

    // stop threads here - actually the last thread will do that
    stop_communication_threads();

    if(!nowait) {
        #pragma omp barrier
    }

    return CHAM_SUCCESS;
}
#else
//!!!! Special version for 2 ranks where rank0 will always offload and rank 2 executes task for testing purposes
int32_t chameleon_distributed_taskwait(int nowait) {
#ifdef TRACE
   static int event_process_remote = -1;
   static const std::string event_process_remote_name = "process_remote";
   if(event_process_remote == -1) {
       int ierr = VT_funcdef(event_process_remote_name.c_str(), VT_NOCLASS, &event_process_remote);
       if(ierr!=VT_OK)
           handle_error_en(ierr, "generate tracing event");
   } 
#endif
    DBP("chameleon_distributed_taskwait (enter)\n");
    verify_initialized();

    // start communication threads here
    start_communication_threads();

    bool had_local_tasks = !_local_tasks.empty();
   
    // as long as there are local tasks run this loop
    while(true) {
        int32_t res = CHAM_SUCCESS;
       
        // ========== Prio 3: work on local tasks
        if(!_local_tasks.empty()) {
            TargetTaskEntryTy *cur_task = chameleon_pop_task();
            if(cur_task == nullptr)
            {
                continue;
            }

            // force offloading to test MPI communication process (will be done by comm thread later)
            if(cur_task) {
                // create temp entry and offload
                OffloadEntryTy *off_entry = new OffloadEntryTy(cur_task, 1);
                res = offload_task_to_rank(off_entry);
            }
        } else {
            break;
        }
    }

    // rank 0 should wait until data comes back
    if(had_local_tasks) {
        while(_num_local_tasks_outstanding > 0)
        {
            usleep(1000);
        }
        // P0: for now return after offloads
        stop_communication_threads();
        return CHAM_SUCCESS;
    }

   
    // P1: call function to recieve remote tasks
    while(true) {
        // only abort if load exchange has happened at least once and there are no outstanding jobs left
        if(_comm_thread_load_exchange_happend && _outstanding_jobs_sum == 0) {
            break;
        }
        if(!_stolen_remote_tasks.empty()) {
#ifdef TRACE
            VT_begin(event_process_remote);
#endif
            int32_t res = process_remote_task();
#ifdef TRACE
            VT_end(event_process_remote);
#endif
        }
        // sleep for 1 ms
        usleep(1000);
    }
    stop_communication_threads();
   
    // TODO: need an implicit OpenMP or MPI barrier here?

    return CHAM_SUCCESS;
}
#endif

int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size) {
    DBP("chameleon_submit_data (enter) - tgt_ptr: " DPxMOD ", hst_ptr: " DPxMOD ", size: %ld\n", DPxPTR(tgt_ptr), DPxPTR(hst_ptr), size);
    verify_initialized();
    // check list if already in
    _mtx_data_entry.lock();
    int found = 0;
    for(auto &entry : _data_entries) {
        if(entry->tgt_ptr == tgt_ptr && entry->hst_ptr == hst_ptr && entry->size == size) {
            // increase reference count
            entry->ref_count++;
            found = 1;
            DBP("chameleon_submit_data - incremented ref count to %d\n", entry->ref_count);
            break;
        }
    }
    if(!found) {
        DBP("chameleon_submit_data - create new entry\n");
        // add it to list
        OffloadingDataEntryTy *new_entry = new OffloadingDataEntryTy(tgt_ptr, hst_ptr, size);
        // printf("Creating new Data Entry with address (" DPxMOD ")\n", DPxPTR(&new_entry));
        _data_entries.push_back(new_entry);
    }
    _mtx_data_entry.unlock();
    DBP("chameleon_submit_data (exit)\n");
    return CHAM_SUCCESS;
}

int32_t chameleon_add_task(TargetTaskEntryTy *task) {
    DBP("chameleon_add_task (enter) - task_entry: " DPxMOD "(idx:%d;offset:%d)\n", DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset);
    verify_initialized();
    // perform lookup in both cases (local execution & offload)
    lookup_hst_pointers(task);

    _mtx_local_tasks.lock();
    // add to queue
    _local_tasks.push_back(task);
    _mtx_local_tasks.unlock();

    _mtx_load_exchange.lock();
    _load_info_local++;
    _num_local_tasks_outstanding++;
    trigger_update_outstanding();
    _mtx_load_exchange.unlock();

    return CHAM_SUCCESS;
}

TargetTaskEntryTy* chameleon_pop_task() {
    DBP("chameleon_pop_task (enter)\n");
    TargetTaskEntryTy* task = nullptr;

    // we do not necessarily need the lock here
    if(_local_tasks.empty()) {
        DBP("chameleon_pop_task (exit)\n");
        return task;
    }

    // get first task in queue
    _mtx_local_tasks.lock();
    if(!_local_tasks.empty()) {
        task = _local_tasks.front();
        _local_tasks.pop_front();
    }
    _mtx_local_tasks.unlock();
    DBP("chameleon_pop_task (exit)\n");
    return task;
}

int32_t lookup_hst_pointers(TargetTaskEntryTy *task) {
    DBP("lookup_hst_pointers (enter) - task_entry: " DPxMOD "\n", DPxPTR(task->tgt_entry_ptr));
    for(int i = 0; i < task->arg_num; i++) {
        // get type and pointer
        int64_t tmp_type    = task->arg_types[i];
        void * tmp_tgt_ptr  = task->arg_tgt_pointers[i];
        int is_lit      = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        int is_from     = tmp_type & CHAM_OMP_TGT_MAPTYPE_FROM;

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
                printf("Error: lookup_hst_pointers - Cannot find mapping for arg_tgt: " DPxMOD ", type: %ld, literal: %d, from: %d\n", 
                    DPxPTR(tmp_tgt_ptr),
                    tmp_type,
                    is_lit,
                    is_from);
                return CHAM_FAILURE;
            } else {
                print_arg_info_w_tgt("lookup_hst_pointers", task, i);
            }
        }
    }

    // clean up data entries again to avoid problems
    _mtx_data_entry.lock();
    for(int i = 0; i < task->arg_num; i++) {
        // get type and pointer
        int64_t tmp_type    = task->arg_types[i];
        void * tmp_tgt_ptr  = task->arg_tgt_pointers[i];
        int is_lit          = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;

        if(!(tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL)) {
            for(auto &entry : _data_entries) {
                if(entry->tgt_ptr == tmp_tgt_ptr) {
                    // remove it if found
                    _data_entries.remove(entry);
                    break;
                }
            }   
        }
    }
    _mtx_data_entry.unlock();

    DBP("lookup_hst_pointers (exit)\n");
    return CHAM_SUCCESS;
}

// int32_t add_offload_entry(TargetTaskEntryTy *task, int rank) {
//     DBP("add_offload_entry (enter) - task_entry: " DPxMOD ", rank: %d\n", DPxPTR(task->tgt_entry_ptr), rank);
//     // create new entry
//     OffloadEntryTy *new_entry = new OffloadEntryTy(task, rank);

//     // save in list
//     _mtx_offload_entries.lock();
//     _offload_entries.push_back(new_entry);
//     _mtx_offload_entries.unlock();

//     return CHAM_SUCCESS;
// }

int32_t execute_target_task(TargetTaskEntryTy *task) {
    DBP("execute_target_task (enter) - task_entry: " DPxMOD "\n", DPxPTR(task->tgt_entry_ptr));
    // Use libffi to launch execution.
    ffi_cif cif;

    // All args are references.
    std::vector<ffi_type *> args_types(task->arg_num, &ffi_type_pointer);

    std::vector<void *> args(task->arg_num);
    std::vector<void *> ptrs(task->arg_num);

    for (int32_t i = 0; i < task->arg_num; ++i) {
        // int64_t tmp_type        = task->arg_types[i];
        // int64_t is_literal      = (tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL);
        // int64_t is_implicit     = (tmp_type & CHAM_OMP_TGT_MAPTYPE_IMPLICIT);
        // int64_t is_to           = (tmp_type & CHAM_OMP_TGT_MAPTYPE_TO);
        // int64_t is_from         = (tmp_type & CHAM_OMP_TGT_MAPTYPE_FROM);
        // int64_t is_prt_obj      = (tmp_type & CHAM_OMP_TGT_MAPTYPE_PTR_AND_OBJ);
        
        // always apply offset in case of array sections
        ptrs[i] = (void *)((intptr_t)task->arg_hst_pointers[i] + task->arg_tgt_offsets[i]);
        args[i] = &ptrs[i];
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
    DBP("process_remote_task (enter)\n");
    TargetTaskEntryTy *remote_task = nullptr;

    if(_stolen_remote_tasks.empty())
        return CHAM_REMOTE_TASK_NONE;
        
    _mtx_stolen_remote_tasks.lock();
    // for safety need to check again after lock is aquired
    if(_stolen_remote_tasks.empty()) {
        _mtx_stolen_remote_tasks.unlock();
        return CHAM_REMOTE_TASK_NONE;
    }
    
    remote_task = _stolen_remote_tasks.front();
    _stolen_remote_tasks.pop_front();
    _mtx_stolen_remote_tasks.unlock();

    // execute region now
#if CHAM_STATS_RECORD
    double cur_time = omp_get_wtime();
#endif
    int32_t res = execute_target_task(remote_task);
#if CHAM_STATS_RECORD
    cur_time = omp_get_wtime()-cur_time;
    _mtx_time_task_execution_stolen.lock();
    _time_task_execution_stolen_sum += cur_time;
    _time_task_execution_stolen_count++;
    _mtx_time_task_execution_stolen.unlock();
#endif
    if(res != CHAM_SUCCESS)
        handle_error_en(1, "execute_target_task - remote");

    // decrement load counter
    _mtx_load_exchange.lock();
    _load_info_local--;
    trigger_update_outstanding();
    _mtx_load_exchange.unlock();

#if OFFLOAD_BLOCKING
    _mtx_offload_blocked.lock();
    _offload_blocked = 0;
    _mtx_offload_blocked.unlock();
#endif

    if(remote_task->HasAtLeastOneOutput()) {
        // just schedule it for sending back results if there is at least 1 output
        _mtx_stolen_remote_tasks_send_back.lock();
        _stolen_remote_tasks_send_back.push_back(remote_task);
        _mtx_stolen_remote_tasks_send_back.unlock();
    } else {
        // we can now decrement outstanding counter because there is nothing to send back
        _mtx_load_exchange.lock();
        _num_stolen_tasks_outstanding--;
        trigger_update_outstanding();
        _mtx_load_exchange.unlock();
    }

#if CHAM_STATS_RECORD
    _mtx_num_executed_tasks_stolen.lock();
    _num_executed_tasks_stolen++;
    _mtx_num_executed_tasks_stolen.unlock();
#endif

    return CHAM_REMOTE_TASK_SUCCESS;
}

#ifdef __cplusplus
}
#endif
