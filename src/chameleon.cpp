#include <ffi.h>
#include <mpi.h>
#include <stdexcept>
#include <algorithm>

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

// ================================================================================
// Variables
// ================================================================================
std::mutex _mtx_relp;
#ifdef CHAM_DEBUG
std::atomic<long> mem_allocated;
#endif
// flag that tells whether library has already been initialized
std::mutex _mtx_ch_is_initialized;
int32_t _ch_is_initialized = 0;
// atomic counter for task ids
std::atomic<int32_t> _task_id_counter(0);

// id of last task that has been created by thread.
// this is thread local storage
__thread int32_t __last_task_id_added = -1;

// list that holds task ids (created at the current rank) that are not finsihed yet
std::mutex _mtx_unfinished_locally_created_tasks;
std::list<int32_t> _unfinished_locally_created_tasks;

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

/* 
 * Function verify_initialized
 * Verifies whether library has already been initialized or not.
 * Otherwise it will throw an error. 
 */
inline void verify_initialized() {
    if(!_ch_is_initialized)
        throw std::runtime_error("Chameleon has not been initilized before.");
}

/* 
 * Function chameleon_init
 * Initialized chameleon library, communicators and all whats necessary.
 */
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
#ifdef CHAM_DEBUG
    mem_allocated = 0;
#endif

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
    _task_id_counter = 0;

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

/*
 * Function chameleon_incr_mem_alloc
 * Increment counter that measures how much memory is allocated for mapped types
 */
void chameleon_incr_mem_alloc(int64_t size) {
#ifdef CHAM_DEBUG
    mem_allocated += size;
#endif
}

/*
 * Function chameleon_set_image_base_address
 * Sets base address of particular image index.
 * This is necessary to determine the entry point for functions that represent a target construct
 */
int32_t chameleon_set_image_base_address(int idx_image, intptr_t base_address) {
    if(_image_base_addresses.size() < idx_image+1) {
        _image_base_addresses.resize(idx_image+1);
    }
    // set base address for image (last device wins)
    DBP("chameleon_set_image_base_address (enter) Setting base_address: " DPxMOD " for img: %d\n", DPxPTR((void*)base_address), idx_image);
    _image_base_addresses[idx_image] = base_address;
    return CHAM_SUCCESS;
}

/* 
 * Function chameleon_finalize
 * Finalizing and cleaning up chameleon library before the program ends.
 */
int32_t chameleon_finalize() {
    DBP("chameleon_finalize (enter)\n");
    verify_initialized();
    DBP("chameleon_finalize (exit)\n");
    return CHAM_SUCCESS;
}

#if !FORCE_OFFLOAD_MASTER_WORKER
/* 
 * Function chameleon_distributed_taskwait
 * Default distributed task wait function that will
 *      - start communication threads
 *      - execute local and stolen tasks
 *      - wait until all global work is done
 * 
 * Also provides the possibility for a nowait if there is a delay caused by stopping the comm threads
 */
int32_t chameleon_distributed_taskwait(int nowait) {
#ifdef TRACE
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
            res = process_remote_task();

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
                atomic_add_dbl(_time_task_execution_local_sum, cur_time);
                _time_task_execution_local_count++;
#endif
                // mark locally created task finished
                _mtx_unfinished_locally_created_tasks.lock();
                _unfinished_locally_created_tasks.remove(cur_task->task_id);
                _mtx_unfinished_locally_created_tasks.unlock();

                // it is save to decrement counter after local execution
                _mtx_load_exchange.lock();
                _num_local_tasks_outstanding--;
                _load_info_local--;
                trigger_update_outstanding();
                _mtx_load_exchange.unlock();

#if OFFLOAD_BLOCKING
                _offload_blocked = 0;
#endif

#if CHAM_STATS_RECORD
                _num_executed_tasks_local++;
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
// //!!!! Special version for 2 ranks where rank0 will always offload and rank 2 executes task for testing purposes
// int32_t chameleon_distributed_taskwait(int nowait) {
//     DBP("chameleon_distributed_taskwait (enter)\n");
//     verify_initialized();

//     // start communication threads here
//     start_communication_threads();

//     bool had_local_tasks = !_local_tasks.empty();
   
//     // as long as there are local tasks run this loop
//     while(true) {
//         int32_t res = CHAM_SUCCESS;
       
//         // ========== Prio 3: work on local tasks
//         if(!_local_tasks.empty()) {
//             TargetTaskEntryTy *cur_task = chameleon_pop_task();
//             if(cur_task == nullptr)
//             {
//                 continue;
//             }

//             // force offloading to test MPI communication process (will be done by comm thread later)
//             if(cur_task) {
//                 // create temp entry and offload
//                 OffloadEntryTy *off_entry = new OffloadEntryTy(cur_task, 1);
//                 res = offload_task_to_rank(off_entry);
//             }
//         } else {
//             break;
//         }
//     }

//     // rank 0 should wait until data comes back
//     if(had_local_tasks) {
//         while(_num_local_tasks_outstanding > 0)
//         {
//             usleep(1000);
//         }
//         // P0: for now return after offloads
//         stop_communication_threads();
//         return CHAM_SUCCESS;
//     }

   
//     // P1: call function to recieve remote tasks
//     while(true) {
//         // only abort if load exchange has happened at least once and there are no outstanding jobs left
//         if(_comm_thread_load_exchange_happend && _outstanding_jobs_sum == 0) {
//             break;
//         }
//         if(!_stolen_remote_tasks.empty()) {
//             int32_t res = process_remote_task();
//         }
//         // sleep for 1 ms
//         usleep(1000);
//     }
//     stop_communication_threads();
   
//     // TODO: need an implicit OpenMP or MPI barrier here?

//     return CHAM_SUCCESS;
// }
#endif

/* 
 * Function chameleon_submit_data
 * Submit mapped data that will be used by tasks.
 */
int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size) {
    DBP("chameleon_submit_data (enter) - tgt_ptr: " DPxMOD ", hst_ptr: " DPxMOD ", size: %ld\n", DPxPTR(tgt_ptr), DPxPTR(hst_ptr), size);
    verify_initialized();
    // check list if already in
    _mtx_data_entry.lock();
    int found = 0;
    for(auto &entry : _data_entries) {
        if(entry->tgt_ptr == tgt_ptr && entry->hst_ptr == hst_ptr && entry->size == size) {
            found = 1;
            break;
        }
    }
    if(!found) {
        DBP("chameleon_submit_data - new entry for tgt_ptr: " DPxMOD ", hst_ptr: " DPxMOD ", size: %ld\n", DPxPTR(tgt_ptr), DPxPTR(hst_ptr), size);
        // add it to list
        OffloadingDataEntryTy *new_entry = new OffloadingDataEntryTy(tgt_ptr, hst_ptr, size);
        // printf("Creating new Data Entry with address (" DPxMOD ")\n", DPxPTR(&new_entry));
        _data_entries.push_back(new_entry);
    }
    _mtx_data_entry.unlock();
    DBP("chameleon_submit_data (exit)\n");
    return CHAM_SUCCESS;
}

void chameleon_free_data(void *tgt_ptr) {
    _mtx_data_entry.lock();
    for(auto &entry : _data_entries) {
        if(entry->tgt_ptr == tgt_ptr) {
            free(entry->tgt_ptr);
#ifdef CHAM_DEBUG
            mem_allocated -= entry->size;
#endif
            _data_entries.remove(entry);
            break;
        }
    }
    _mtx_data_entry.unlock();
}

int32_t chameleon_add_task(TargetTaskEntryTy *task) {
    DBP("chameleon_add_task (enter) - task_entry (task_id=%d): " DPxMOD "(idx:%d;offset:%d) with arg_num: %d\n", task->task_id, DPxPTR(task->tgt_entry_ptr), task->idx_image, (int)task->entry_image_offset, task->arg_num);
    verify_initialized();
    // perform lookup in both cases (local execution & offload)
    lookup_hst_pointers(task);

    // add to queue
    _mtx_local_tasks.lock();
    _local_tasks.push_back(task);
    // set last task added
    __last_task_id_added = task->task_id;
    _mtx_unfinished_locally_created_tasks.lock();
    _unfinished_locally_created_tasks.push_back(task->task_id);
    _mtx_unfinished_locally_created_tasks.unlock();
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

int32_t chameleon_get_last_local_task_id_added() {
    return __last_task_id_added;
}

/*
 * Checks whether the corresponding task has already been finished
 */
int32_t chameleon_local_task_has_finished(int32_t task_id) {
    _mtx_unfinished_locally_created_tasks.lock();
    bool found = (std::find(_unfinished_locally_created_tasks.begin(), _unfinished_locally_created_tasks.end(), task_id) != _unfinished_locally_created_tasks.end());
    _mtx_unfinished_locally_created_tasks.unlock();
    return found ? 0 : 1;
}

int32_t lookup_hst_pointers(TargetTaskEntryTy *task) {
    DBP("lookup_hst_pointers (enter) - task_entry (task_id=%d): " DPxMOD "\n", task->task_id, DPxPTR(task->tgt_entry_ptr));
    for(int i = 0; i < task->arg_num; i++) {
        // get type and pointer
        int64_t tmp_type    = task->arg_types[i];
        void * tmp_tgt_ptr  = task->arg_tgt_pointers[i];
        int is_lit          = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;
        int is_from         = tmp_type & CHAM_OMP_TGT_MAPTYPE_FROM;

        if(tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL) {
            // pointer represents numerical value that is implicitly mapped
            task->arg_hst_pointers[i] = tmp_tgt_ptr;
            task->arg_sizes[i] = sizeof(void *);
            print_arg_info_w_tgt("lookup_hst_pointers", task, i);
        } else {
            // here we need to perform a pointer mapping to host pointer
            // because target pointers have already been freed and deleted
            int found = 0;
            _mtx_data_entry.lock();
            for(auto &entry : _data_entries) {
                // printf("Checking Mapping Entry (" DPxMOD ")\n", DPxPTR(entry.tgt_ptr));
                if(entry->tgt_ptr == tmp_tgt_ptr) {
                    // if(!found) {
                        task->arg_sizes[i] = entry->size;
                        task->arg_hst_pointers[i] = entry->hst_ptr;
                        print_arg_info_w_tgt("lookup_hst_pointers (first)", task, i);
                    // } else {
                    //     // temporary assignment to find more matches and errors
                    //     void * swap = task->arg_hst_pointers[i];
                    //     task->arg_hst_pointers[i] = entry->hst_ptr;
                    //     print_arg_info_w_tgt("lookup_hst_pointers (next)", task, i);
                    //     task->arg_hst_pointers[i] = swap;
                    // }
                    found = 1;
                    break;
                }
            }
            _mtx_data_entry.unlock();
            if(!found) {
                // There seems to be a race condition in internal mapping von source to target pointers (that might be reused) and the kernel call if using multiple threads 
                // Workaround: wait a small amount of time and try again once more (i know it is ugly but hard to fix that race here)
                usleep(1000);
                _mtx_data_entry.lock();
                for(auto &entry : _data_entries) {
                    // printf("Checking Mapping Entry (" DPxMOD ")\n", DPxPTR(entry.tgt_ptr));
                    if(entry->tgt_ptr == tmp_tgt_ptr) {
                        task->arg_sizes[i] = entry->size;
                        task->arg_hst_pointers[i] = entry->hst_ptr;
                        found = 1;
                        break;
                    }
                }
                _mtx_data_entry.unlock();
            }
            if(!found) {
                // something went wrong here
                RELP("Error: lookup_hst_pointers - Cannot find mapping for arg_tgt: " DPxMOD ", type: %ld, literal: %d, from: %d\n", 
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

    // // clean up data entries again to avoid problems
    // _mtx_data_entry.lock();
    // for(int i = 0; i < task->arg_num; i++) {
    //     // get type and pointer
    //     int64_t tmp_type    = task->arg_types[i];
    //     void * tmp_tgt_ptr  = task->arg_tgt_pointers[i];
    //     int is_lit          = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;

    //     if(!(tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL)) {
    //         for(auto &entry : _data_entries) {
    //             if(entry->tgt_ptr == tmp_tgt_ptr) {
    //                     // remove it if found
    //                     free(entry->tgt_ptr);
    //                     mem_allocated -= entry->size;
    //                     _data_entries.remove(entry);
    //                 break;
    //             }
    //         }   
    //     }
    // }
    // _mtx_data_entry.unlock();

    DBP("lookup_hst_pointers (exit)\n");
    return CHAM_SUCCESS;
}

int32_t execute_target_task(TargetTaskEntryTy *task) {
    DBP("execute_target_task (enter) - task_entry (task_id=%d): " DPxMOD "\n", task->task_id, DPxPTR(task->tgt_entry_ptr));
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

#ifdef TRACE
    static int event_process_remote = -1;
    static const std::string event_process_remote_name = "process_remote";
    if( event_process_remote == -1) 
        int ierr = VT_funcdef(event_process_remote_name.c_str(), VT_NOCLASS, &event_process_remote);
    VT_begin(event_process_remote);
#endif
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
    atomic_add_dbl(_time_task_execution_stolen_sum, cur_time);
    _time_task_execution_stolen_count++;
#endif
    if(res != CHAM_SUCCESS)
        handle_error_en(1, "execute_target_task - remote");

    // decrement load counter
    _mtx_load_exchange.lock();
    _load_info_local--;
    // trigger_update_outstanding();
    _mtx_load_exchange.unlock();

#if OFFLOAD_BLOCKING
    _offload_blocked = 0;
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
    _num_executed_tasks_stolen++;
#endif
#ifdef TRACE
    VT_end(event_process_remote);
#endif
    return CHAM_REMOTE_TASK_SUCCESS;
}

#ifdef __cplusplus
}
#endif
