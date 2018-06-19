// Chameleon Header
#ifndef _CHAMELEON_H_
#define _CHAMELEON_H_

#ifdef __cplusplus
extern "C" {
#endif

extern int chameleon_comm_rank;
extern int chameleon_comm_size;

int chameleon_init();

int chameleon_finalize();

int chameleon_distributed_taskwait();

#ifdef __cplusplus
}
#endif

#endif
