#ifndef _CHAMELEON_COMMON_H_
#define _CHAMELEON_COMMON_H_
// common includes or definintion of internal data structures

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <omp.h>
#include <errno.h>
#include <inttypes.h>
#include <iterator>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sstream>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef DPxMOD
#define DPxMOD "0x%0*" PRIxPTR
#endif

#ifndef DPxPTR
#define DPxPTR(ptr) ((int)(2*sizeof(uintptr_t))), ((uintptr_t) (ptr))
#endif

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

static void chameleon_dbg_print_help(int print_prefix, const char * prefix, int rank, va_list args) {
    timeval curTime;
    gettimeofday(&curTime, NULL);
    int milli = curTime.tv_usec / 1000;
    int micro_sec = curTime.tv_usec % 1000;
    char buffer [80];
    strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", localtime(&curTime.tv_sec));
    char currentTime[84] = "";
    sprintf(currentTime, "%s.%03d.%03d", buffer, milli, micro_sec);

    _mtx_relp.lock();
    if(print_prefix)
        fprintf(stderr, "%s %s R#%d T#%d (OS_TID:%ld): --> ", currentTime, prefix, rank, omp_get_thread_num(), syscall(SYS_gettid));
    // fprintf(stderr, "%s ChameleonLib R#%d T#%d (OS_TID:%ld): --> ", currentTime, rank, omp_get_thread_num(), syscall(SYS_gettid));
    // get format (first element from var args)
    const char *fmt = va_arg(args, const char *);
    vfprintf(stderr, fmt, args);
    _mtx_relp.unlock();
}

static void chameleon_dbg_print(int rank, ... ) {
    va_list args;
    va_start(args, rank);
    chameleon_dbg_print_help(1, "ChameleonLib", rank, args);
    va_end (args);
}

#ifndef RELP
#define RELP( ... ) chameleon_dbg_print(chameleon_comm_rank, __VA_ARGS__);
#endif

#ifndef DBP
#ifdef CHAM_DEBUG
#define DBP( ... ) { RELP(__VA_ARGS__); }
#else
#define DBP( ... ) { }
#endif
#endif

#define handle_error_en(en, msg) \
           do { errno = en; RELP("ERROR: %s : %s\n", msg, strerror(en)); exit(EXIT_FAILURE); } while (0)

template <class Container>
static void split_string(const std::string& str, Container& cont, char delim = ' ')
{
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delim)) {
        cont.push_back(token);
    }
}
#endif
