#include "chameleon.h"
#include <stdio.h>
#include <stdlib.h>

MPI_Comm chameleon_comm;
int chameleon_comm_rank;
int chameleon_comm_size;

int chameleon_init() {
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

    printf("Chameleon: Hello from rank is %d of %d\n", chameleon_comm_rank, chameleon_comm_size);

    // TODO: create communication thread (maybe start later)

    return 0;
}

int chameleon_finalize() {
    return 0;
}

int chameleon_distributed_taskwait() {
    return 0;
}