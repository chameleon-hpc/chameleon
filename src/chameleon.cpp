#include "chameleon.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mpi.h>

#include <list>
// #include <thread>         // std::thread
#include <mutex>          // std::mutex

std::mutex _mtx_data_entry;
std::list<OffloadingDataEntryTy> _data_entries;

MPI_Comm chameleon_comm;
int chameleon_comm_rank;
int chameleon_comm_size;

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
    #pragma omp target device(1001) // 1001 = CHAMELEON_HOST
    {
    }

    // TODO: create communication thread (maybe start later)

    return 0;
}

int32_t chameleon_finalize() {
    return 0;
}

int32_t chameleon_distributed_taskwait() {
    // TODO: remember to remove entries from _data_entries as soon as reference count hits 0
    return 0;
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
    return 0;
}

int32_t chameleon_submit_implicit_data(void *tgt_ptr) {
    // check whether matching mapped entry already in lookup table
    _mtx_data_entry.lock();
    for(auto &entry : _data_entries) {
        if(entry.tgt_ptr == tgt_ptr && entry.is_implicit == 0) {
            // there is already an entry that has explicitly been mapped
            return 0;
        }
    }

    // copy value that is represented by pointer and insert in table + mark as implicit (can be deleted after work package)
    // Q: we currently assume that each agrument has 8 Byte.. Only addresses, some of which represent an address and some an actual value
    // Calling interface is not providing information about size of arguments

    // pointer value is not really a pointer but representing a numeric value
    double tt = *(double*)&tgt_ptr;
    double * pp = (double *) malloc(sizeof(double));
    *pp = tt;
    void * vpp = (void *)pp;

    // new entry + mark as implicit
    // OffloadingDataEntryTy new_entry(tgt_ptr, vpp, 8);
    // new_entry.is_implicit = 1;
    // add to list
    // _data_entries.push_back(new_entry);
    _mtx_data_entry.unlock();
    return 0;
}

#ifdef __cplusplus
}
#endif
