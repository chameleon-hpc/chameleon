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

#include <cstdint>
#include <list>
#include <mutex>
#include <vector>
#include <atomic>

#include "chameleon.h"

// Special version with 2 ranks where master (rank 0) is always offloading to rank 1
#ifndef FORCE_OFFLOAD_MASTER_WORKER
#define FORCE_OFFLOAD_MASTER_WORKER 0
#endif

// Flag wether offloading in general is enabled or disabled
#ifndef OFFLOAD_ENABLED
#define OFFLOAD_ENABLED 1
#endif

// Allow offload as soon as sum of outstanding jobs has changed
#ifndef OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED
#define OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED 1
#endif

// Just allow a single offload and block offload until a local or remote task has been executed and the local load has changed again
#ifndef OFFLOAD_BLOCKING
#define OFFLOAD_BLOCKING 0
#endif

// determines how data (arguments) is packed and send during offloading
#ifndef OFFLOAD_DATA_PACKING_TYPE
// #define OFFLOAD_DATA_PACKING_TYPE 0     // 0 = pack meta data and arguments together and send it with a single message (requires copy to buffer)
#define OFFLOAD_DATA_PACKING_TYPE 1     // 1 = zero copy approach, only pack meta data (num_args, arg types ...) + separat send for each mapped argument
#endif

// Create a separate thread for offloads that expect mapped data to be transfered back
#ifndef OFFLOAD_CREATE_SEPARATE_THREAD
#define OFFLOAD_CREATE_SEPARATE_THREAD 0
#endif

#ifndef THREAD_ACTIVATION
#define THREAD_ACTIVATION 1
#endif

//Specify whether blocking or non-blocking MPI should be used (blocking in the sense of MPI_Isend or MPI_Irecv followed by an MPI_Waitall)
#ifndef MPI_BLOCKING
#define MPI_BLOCKING 0
#endif

//Specify whether tasks should be offloaded aggressively after one performance update
#ifndef OFFLOADING_STRATEGY_AGGRESSIVE
#define OFFLOADING_STRATEGY_AGGRESSIVE 0
#endif

#ifndef CHAMELEON_TOOL_SUPPORT
#define CHAMELEON_TOOL_SUPPORT 1
#endif

#ifndef CHAMELEON_TOOL_USE_MAP
#define CHAMELEON_TOOL_USE_MAP 0
#endif

#if CHAMELEON_TOOL_SUPPORT
#include "chameleon_tools.h"
#endif

#pragma region Type Definitions
typedef struct TargetTaskEntryTy {
    // target entry point / function that holds the code that should be executed
    intptr_t tgt_entry_ptr;
    // we need index of image here as well since pointers are not matching for other ranks
    int32_t idx_image = 0;
    ptrdiff_t entry_image_offset = 0;

    // task id (unique id that combines the host rank and a unique id per rank)
    int32_t task_id;

    // number of arguments that should be passed to "function call" for target region
    int32_t arg_num;

    // host pointers will be used for transfer execution target region
    std::vector<void *> arg_hst_pointers;
    std::vector<int64_t> arg_sizes;
    std::vector<int64_t> arg_types;

    // target pointers will just be used at sender side for host pointer lookup 
    // and freeing of entries in data entry table
    std::vector<void *> arg_tgt_pointers;
    std::vector<ptrdiff_t> arg_tgt_offsets;

    int32_t is_remote_task      = 0;
    int32_t is_replicated_task  = 0;
    int32_t is_manual_task      = 0;

    // Some special settings for stolen tasks
    int32_t source_mpi_rank     = 0;
    int32_t source_mpi_tag      = 0;

#if CHAMELEON_TOOL_SUPPORT
    cham_t_data_t task_tool_data;
#endif

    // Constructor 1: Called when creating new task during decoding
    // here we dont need to give a task id in that case because it should be transfered from source
    TargetTaskEntryTy() { }

    // Constructor 2: Called from libomptarget plugin to create a new task
    TargetTaskEntryTy(
        void *p_tgt_entry_ptr, 
        void **p_tgt_args, 
        ptrdiff_t *p_tgt_offsets, 
        int64_t *p_tgt_arg_types, 
        int32_t p_arg_num); 

    void ReSizeArrays(int32_t num_args);
    
    int HasAtLeastOneOutput();
    
} TargetTaskEntryTy;

struct OffloadEntryTy {
    TargetTaskEntryTy *task_entry;
    int32_t target_rank;

    OffloadEntryTy(TargetTaskEntryTy *par_task_entry, int32_t par_target_rank) {
        task_entry = par_task_entry;
        target_rank = par_target_rank;
    }
};

struct OffloadingDataEntryTy {
    void *tgt_ptr;
    void *hst_ptr;
    int64_t size;

    OffloadingDataEntryTy() {
        tgt_ptr = nullptr;
        hst_ptr = nullptr;
        size = 0;
    }

    OffloadingDataEntryTy(void *p_tgt_ptr, void *p_hst_ptr, int64_t p_size) {
        tgt_ptr = p_tgt_ptr;
        hst_ptr = p_hst_ptr;
        size = p_size;
    }
};

typedef struct ch_thread_data_t {
    int32_t os_thread_id;
    TargetTaskEntryTy * current_task;

#if CHAMELEON_TOOL_SUPPORT
    cham_t_data_t thread_tool_data;
#endif    
} ch_thread_data_t;

typedef struct ch_rank_data_t {
    int32_t comm_rank;
    int32_t comm_size;

#if CHAMELEON_TOOL_SUPPORT
    cham_t_data_t rank_tool_data;
    cham_t_rank_info_t rank_tool_info;
#endif    
} ch_rank_data_t;

class thread_safe_task_list {
    private:
    
    std::list<TargetTaskEntryTy*> task_list;
    std::mutex m;
    // size_t list_size = 0;
    std::atomic<size_t> list_size = 0;

    public:

    thread_safe_task_list() { }

    size_t size() {
        return this->list_size.load();
    }

    bool empty() {
        return this->list_size <= 0;
    }

    void push_back(TargetTaskEntryTy* task) {
        this->m.lock();
        this->task_list.push_back(task);
        this->list_size++;
        this->m.unlock();
    }

    TargetTaskEntryTy* pop_front() {
        if(this->empty())
            return nullptr;

        TargetTaskEntryTy* ret_val = nullptr;

        this->m.lock();
        if(!this->empty()) {
            this->list_size--;
            ret_val = this->task_list.front();
            this->task_list.pop_front();
        }
        this->m.unlock();
        return ret_val;
    }

    int64_t* get_task_ids(int32_t* num_ids) {
        this->m.lock();
        int64_t *vec = (int64_t *) malloc(this->task_list.size() * sizeof(int64_t));
        *num_ids = this->task_list.size();
        size_t count = 0;
        for (std::list<TargetTaskEntryTy*>::iterator it=this->task_list.begin(); it!=this->task_list.end(); ++it) {
            vec[count] = (*it)->task_id;
            count++;
        }
        this->m.unlock();
        return vec;
    }
};

template<class T>
class thread_safe_list {
    private:
    
    std::list<T> list;
    std::mutex m;
    // size_t list_size = 0;
    std::atomic<size_t> list_size = 0;

    public:

    thread_safe_list() { }

    size_t size() {
        return this->list_size.load();
    }

    bool empty() {
        return this->list_size <= 0;
    }

    void push_back(T* entry) {
        this->m.lock();
        this->list.push_back(entry);
        this->list_size++;
        this->m.unlock();
    }

    T pop_front() {
        if(this->empty())
            return NULL;

        this->m.lock();
        if(!this->empty()) {
            this->list_size--;
            ret_val = this->list.front();
            this->list.pop_front();
        }
        this->m.unlock();
        return ret_val;
    }
};
#pragma endregion

#pragma region Variables
extern int chameleon_comm_rank;
extern int chameleon_comm_size;

// atomic counter for task ids
extern std::atomic<int32_t> _task_id_counter;

// atomic counter for thread ids
extern std::atomic<int32_t> _thread_counter;
extern __thread int __ch_gtid;
extern int32_t __ch_get_gtid();

extern std::mutex _mtx_relp;

extern ch_thread_data_t*    __thread_data;
extern ch_rank_data_t       __rank_data;

#ifdef CHAM_DEBUG
extern std::atomic<long> mem_allocated;
#endif
#pragma endregion

#pragma region Functions
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
#pragma endregion

#endif
