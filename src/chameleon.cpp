#include "chameleon.h"
#include <ffi.h>
#include <list>
#include <mpi.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

static std::mutex _mtx_data_entry;
static std::list<OffloadingDataEntryTy> _data_entries;
static std::mutex _mtx_tasks;
static std::list<OffloadingTaskEntryTy> _tasks;

static MPI_Comm chameleon_comm;
static int chameleon_comm_rank;
static int chameleon_comm_size;

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

    // dummy target region to force binary loading
    // currently the 
    #pragma omp target device(1001) // 1001 = CHAMELEON_HOST
    {
    }

    // TODO: create communication thread (maybe start later)

    return CHAM_SUCCESS;
}

int32_t chameleon_finalize() {
    return CHAM_SUCCESS;
}

int32_t chameleon_distributed_taskwait() {
    // TODO: remember to remove entries from _data_entries as soon as reference count hits 0

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

        for (int32_t i = 0; i < cur_task.arg_num; ++i) {
            ptrs[i] = (void *)((intptr_t)cur_task.tgt_args[i] + cur_task.tgt_offsets[i]);
            args[i] = &ptrs[i];

            // TODO: here we need to perform a pointer mapping to source pointers 
            // because target pointers have already been deleted
            if(cur_task.tgt_arg_types[i] && CH_OMP_TGT_MAPTYPE_LITERAL) {
                // no need to do anything because it is by value
                continue;
            }

            // get source pointer for tgt_ptr
            int found = 0;
            for(auto &entry : _data_entries) {
                if(entry.tgt_ptr == args[i]) {
                    // increase reference count
                    args[i] = entry.hst_ptr;
                    found = 1;
                    break;
                }
            }
            if(!found) {
                // something went wrong here
            }
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

// int32_t chameleon_submit_implicit_data(void *tgt_ptr) {
//     // check whether matching mapped entry already in lookup table
//     _mtx_data_entry.lock();
//     for(auto &entry : _data_entries) {
//         if(entry.tgt_ptr == tgt_ptr && entry.is_implicit == 0) {
//             // there is already an entry that has explicitly been mapped
//             _mtx_data_entry.unlock();
//             return 0;
//         }
//     }

//     // copy value that is represented by pointer and insert in table + mark as implicit (can be deleted after work package)
//     // Q: we currently assume that each agrument has 8 Byte.. Only addresses, some of which represent an address and some an actual value
//     // Calling interface is not providing information about size of arguments

//     // pointer value is not really a pointer but representing a numeric value
//     double tt = *(double*)&tgt_ptr;
//     double * pp = (double *) malloc(sizeof(double));
//     *pp = tt;
//     void * vpp = (void *)pp;

//     // new entry + mark as implicit
//     // OffloadingDataEntryTy new_entry(tgt_ptr, vpp, 8);
//     // new_entry.is_implicit = 1;
//     // add to list
//     // _data_entries.push_back(new_entry);
//     _mtx_data_entry.unlock();
//     return CHAM_SUCCESS;
// }

#ifdef __cplusplus
}
#endif
