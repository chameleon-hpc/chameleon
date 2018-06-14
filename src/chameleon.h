// Chameleon Header
#ifndef _CHAMELEON_H_
#define _CHAMELEON_H_

#include <mpi.h>

extern MPI_Comm __chameleon_communicator;
extern int chameleon_comm_rank;
extern int chameleon_comm_size;

int chameleon_init();

int chameleon_finalize();

int chameleon_distributed_taskwait();

#endif