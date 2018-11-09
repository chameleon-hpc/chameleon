#ifndef _CHAMELEON_COMMON_H_
#define _CHAMELEON_COMMON_H_
// common includes or definintion of internal data structures

#include <errno.h>
#include <inttypes.h>
#include <omp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdint>
#include <list>
#include <mutex>
#include <vector>
#include <atomic>

extern int chameleon_comm_rank;
extern int chameleon_comm_size;

// atomic counter for task ids
extern std::atomic<int32_t> _task_id_counter;

extern std::mutex _mtx_relp;

#ifdef CHAM_DEBUG
extern std::atomic<long> mem_allocated;
#endif

#ifndef DBP
#ifdef CHAM_DEBUG
#define DBP( ... ) { RELP(__VA_ARGS__); }
#else
#define DBP( ... ) { }
#endif
#endif

#ifndef RELP
#define RELP( ... )                                                                                         \
  {                                                                                                        \
     \
     _mtx_relp.lock(); \
    fprintf(stderr, "ChameleonLib R#%d T#%d (OS_TID:%ld): --> ", chameleon_comm_rank, omp_get_thread_num(), syscall(SYS_gettid));      \
    fprintf(stderr, __VA_ARGS__);                                                                           \
    _mtx_relp.unlock(); \
     \
  }
#endif

#define handle_error_en(en, msg) \
           do { errno = en; RELP("ERROR: %s : %s\n", msg, strerror(en)); exit(EXIT_FAILURE); } while (0)
#endif
