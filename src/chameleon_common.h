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
#include <unordered_map>

#include "chameleon.h"

// Flag wether offloading in general is enabled or disabled
#ifndef OFFLOAD_ENABLED
#define OFFLOAD_ENABLED 1
#endif

// Flag whether task migration is forced. That means no locally created task are executed locally
#ifndef FORCE_MIGRATION
#define FORCE_MIGRATION 0
#endif

// Allow offload as soon as sum of outstanding jobs has changed
#ifndef OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED
#define OFFLOAD_AFTER_OUTSTANDING_SUM_CHANGED 1
#endif

// determines how data (arguments) is packed and send during offloading
#ifndef OFFLOAD_DATA_PACKING_TYPE
// #define OFFLOAD_DATA_PACKING_TYPE 0     // 0 = pack meta data and arguments together and send it with a single message (requires copy to buffer)
// #define OFFLOAD_DATA_PACKING_TYPE 1     // 1 = zero copy approach, only pack meta data (num_args, arg types ...) + separat send for each mapped argument
#define OFFLOAD_DATA_PACKING_TYPE 2     // 2 = zero copy approach, only pack meta data (num_args, arg types ...) + ONE separat send for with mapped arguments
#endif

#ifndef OFFLOAD_SEND_TASKS_SEPARATELY
#define OFFLOAD_SEND_TASKS_SEPARATELY 0
#endif

#ifndef THREAD_ACTIVATION
#define THREAD_ACTIVATION 1
#endif

//Specify whether blocking or non-blocking MPI should be used (blocking in the sense of MPI_Isend or MPI_Irecv followed by an MPI_Waitall)
#ifndef MPI_BLOCKING
#define MPI_BLOCKING 0
#endif

#ifndef CHAM_REPLICATION_MODE
#define CHAM_REPLICATION_MODE 0 //no replication
// #define CHAM_REPLICATION_MODE 1 //replicated tasks may be processed locally if needed, however, no remote task cancellation is used
// #define CHAM_REPLICATION_MODE 2 //replicated tasks may be processed locally if needed; remote replica task is cancelled
#endif

//Specify whether tasks should be offloaded aggressively after one performance update
#ifndef OFFLOADING_STRATEGY_AGGRESSIVE
#define OFFLOADING_STRATEGY_AGGRESSIVE 0
#endif

#ifndef CHAMELEON_TOOL_SUPPORT
#define CHAMELEON_TOOL_SUPPORT 0
#endif

#ifndef CHAMELEON_TOOL_USE_MAP
#define CHAMELEON_TOOL_USE_MAP 0
#endif

#ifndef SHOW_WARNING_DEADLOCK
#define SHOW_WARNING_DEADLOCK 0
#endif

#ifndef SHOW_WARNING_SLOW_COMMUNICATION
#define SHOW_WARNING_SLOW_COMMUNICATION 0
#endif

#ifndef PRINT_CONFIG_VALUES
#define PRINT_CONFIG_VALUES 1
#endif

#ifndef ENABLE_TRACING_FOR_SYNC_CYCLES
#define ENABLE_TRACING_FOR_SYNC_CYCLES 0
#endif

#if CHAMELEON_TOOL_SUPPORT
#include "chameleon_tools.h"
#endif

#ifndef CHAM_MIGRATE_ANNOTATIONS
#define CHAM_MIGRATE_ANNOTATIONS 0
#endif

#pragma region Type Definitions
typedef enum cham_annotation_value_type_t {
    cham_annotation_int         = 0,
    cham_annotation_int64       = 1,
    cham_annotation_double      = 2,
    cham_annotation_float       = 3,
    cham_annotation_string      = 4
} cham_annotation_value_type_t;

typedef union cham_annotation_value_t {
    int64_t     val_int64;
    int32_t     val_int32;
    double      val_double;
    float       val_float;
    void*       val_ptr;
} cham_annotation_value_t;

typedef struct cham_annotation_entry_t {
    int32_t                 value_type;
    int32_t                 string_length;
    int32_t                 is_manual_allocated;
    cham_annotation_value_t value;
} cham_annotation_entry_t;

static cham_annotation_entry_t cham_annotation_entry_string(int32_t val_type, int32_t str_len, cham_annotation_value_t val) {
    cham_annotation_entry_t entry;
    entry.value_type            = val_type;
    entry.string_length         = str_len;
    entry.value                 = val;
    entry.is_manual_allocated   = 0;
    return entry;
}

static cham_annotation_entry_t cham_annotation_entry_value(int32_t val_type, cham_annotation_value_t val) {
    cham_annotation_entry_t entry;
    entry.value_type            = val_type;
    entry.string_length         = -1;
    entry.value                 = val;
    entry.is_manual_allocated   = 0;
    return entry;
}

typedef struct chameleon_annotations_t {
    std::unordered_map<std::string, cham_annotation_entry_t> anno;
    chameleon_annotations_t() { }

    void* pack(int32_t* buffer_size) {
        *buffer_size = 0;
        if(anno.empty()) {
            return nullptr;
        }

        int32_t num_annotations = 0;
        *buffer_size += sizeof(int32_t);

        // first get size of buffer
        for (std::pair<std::string, cham_annotation_entry_t> element : anno) {
            if(element.second.value_type == cham_annotation_string) {
                *buffer_size += (
                    sizeof(int32_t)                     // int32_t with size of key string
                    + element.first.length()            // actual size of key string
                    + sizeof(int32_t)                   // value type
                    + sizeof(int32_t)                   // int32_t with size of value string
                    + element.second.string_length);    // actual size of value string
            } else {
                *buffer_size += (
                    sizeof(int32_t)                     // int32_t with size of key string
                    + element.first.length()            // actual size of key string
                    + sizeof(int32_t)                   // value type
                    + sizeof(cham_annotation_value_t)); // actual value
            }            
            num_annotations++;
        }
        
        // allocate buffer
        char* buffer    = (char*) malloc(*buffer_size);
        char* cur_ptr   = (char*) buffer;
        
        ((int32_t *) cur_ptr)[0] = num_annotations;
        cur_ptr += sizeof(int32_t);

        // iterate and serialize values
        for (std::pair<std::string, cham_annotation_entry_t> element : anno) {
            // 1: size of string
            ((int32_t *) cur_ptr)[0] = (int32_t)element.first.length();
            cur_ptr += sizeof(int32_t);
            // 2: bytes for string
            memcpy(cur_ptr, element.first.data(), element.first.length());
            cur_ptr += element.first.length();
            // value type
            ((int32_t *) cur_ptr)[0] = (int32_t)element.second.value_type;
            cur_ptr += sizeof(int32_t);

            if(element.second.value_type == cham_annotation_string) {
                // size of string
                ((int32_t *) cur_ptr)[0] = (int32_t)element.second.string_length;
                cur_ptr += sizeof(int32_t);
                // actual string
                memcpy(cur_ptr, (char*)element.second.value.val_ptr, element.second.string_length);
                cur_ptr += element.second.string_length;
            } else {
                // 3: bytes for value
                ((cham_annotation_value_t *) cur_ptr)[0] = element.second.value;
                cur_ptr += sizeof(cham_annotation_value_t);
            }
        }
        return buffer;
    }

    void unpack(void* buffer) {
        char *cur_ptr = (char*) buffer;

        int32_t num_annotations = ((int32_t *) cur_ptr)[0];
        cur_ptr += sizeof(int32_t);

        for (int i = 0; i < num_annotations; i++) {
            // key size
            int32_t str_size = ((int32_t *) cur_ptr)[0];
            cur_ptr += sizeof(int32_t);
            
            // key string
            char* key = (char*) malloc(str_size+1);
            memcpy(key, cur_ptr, str_size);
            cur_ptr += str_size;
            key[str_size] = '\0';
            std::string cur_key = std::string(key);
            free(key);
            
            // value type
            int32_t value_type = ((int32_t *) cur_ptr)[0];
            cur_ptr += sizeof(int32_t);

            if(value_type == cham_annotation_string) {
                // string size
                int32_t val_size = ((int32_t *) cur_ptr)[0];
                cur_ptr += sizeof(int32_t);
                // actual string value
                char* str_value = (char*) malloc(val_size+1);
                memcpy(str_value, cur_ptr, val_size);
                cur_ptr += val_size;
                str_value[val_size] = '\0';
                cham_annotation_value_t value;
                value.val_ptr = (void*)str_value;
                cham_annotation_entry_t cur_entry = cham_annotation_entry_string(value_type, val_size, value);
                cur_entry.is_manual_allocated = 1;
                anno.insert(std::make_pair(cur_key, cur_entry));
            } else {
                cham_annotation_value_t value = ((cham_annotation_value_t *) cur_ptr)[0];
                cur_ptr += sizeof(cham_annotation_value_t);
                anno.insert(std::make_pair(cur_key, cham_annotation_entry_value(value_type, value)));
            }
        }
    }

    void free_annotations() {
        for (std::pair<std::string, cham_annotation_entry_t> element : anno) {
            if(element.second.value_type == cham_annotation_string && element.second.is_manual_allocated) {
                free(element.second.value.val_ptr);
            }
        }
    }

} chameleon_annotations_t;

typedef struct cham_migratable_task_t {
    // target entry point / function that holds the code that should be executed
    intptr_t tgt_entry_ptr;
    // we need index of image here as well since pointers are not matching for other ranks
    int32_t idx_image = 0;
    ptrdiff_t entry_image_offset = 0;

    // task id (unique id that combines the host rank and a unique id per rank)
    TYPE_TASK_ID task_id;

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
    int32_t is_manual_task      = 0;
    int32_t is_replicated_task  = 0;

    int32_t num_outstanding_recvbacks = 0;

    // Some special settings for stolen tasks
    int32_t source_mpi_rank     = 0;
    int32_t target_mpi_rank     = -1;

    // Mutex for either execution or receiving back/cancellation of a replicated task
    std::atomic<bool> result_in_progress;

    // Vector of replicating ranks
    std::vector<int> replication_ranks;

    chameleon_annotations_t* task_annotations = nullptr;

#if CHAMELEON_TOOL_SUPPORT
    cham_t_data_t task_tool_data;
#endif

    // Constructor 1: Called when creating new task during decoding or from application with API call
    // here we dont need to give a task id in that case because it should be transfered from source
    cham_migratable_task_t() : result_in_progress(false) { }

    // Constructor 2: Called from libomptarget plugin to create a new task
    cham_migratable_task_t(
        void *p_tgt_entry_ptr, 
        void **p_tgt_args, 
        ptrdiff_t *p_tgt_offsets, 
        int64_t *p_tgt_arg_types, 
        int32_t p_arg_num);

    void ReSizeArrays(int32_t num_args);
    
    int HasAtLeastOneOutput();
    
} cham_migratable_task_t;

typedef struct migratable_data_entry_t {
    void *tgt_ptr;
    void *hst_ptr;
    int64_t size;

    migratable_data_entry_t() {
        tgt_ptr = nullptr;
        hst_ptr = nullptr;
        size = 0;
    }

    migratable_data_entry_t(void *p_tgt_ptr, void *p_hst_ptr, int64_t p_size) {
        tgt_ptr = p_tgt_ptr;
        hst_ptr = p_hst_ptr;
        size = p_size;
    }
} migratable_data_entry_t;

typedef struct ch_thread_data_t {
    int32_t os_thread_id;
    cham_migratable_task_t * current_task;

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

class thread_safe_task_map_t {
    private:
    
    std::unordered_map<TYPE_TASK_ID, cham_migratable_task_t*> task_map;
    std::mutex m;
    std::atomic<size_t> map_size;

    public:

    thread_safe_task_map_t() { map_size = 0; }

    size_t size() {
        return this->map_size.load();
    }

    bool empty() {
        return this->map_size <= 0;
    }

    void insert(TYPE_TASK_ID task_id, cham_migratable_task_t* task) {
        this->m.lock();
        this->task_map.insert(std::make_pair(task_id, task));
        this->map_size++;
        this->m.unlock();
    }

    void erase(TYPE_TASK_ID task_id) {
        this->m.lock();
        this->task_map.erase(task_id);
        this->map_size--;
        this->m.unlock();
    }

    cham_migratable_task_t* find(TYPE_TASK_ID task_id) {
        cham_migratable_task_t* val = nullptr;
        this->m.lock();
        std::unordered_map<TYPE_TASK_ID ,cham_migratable_task_t*>::const_iterator got = this->task_map.find(task_id);
        if(got != this->task_map.end()) {
            val = got->second;
        }
        this->m.unlock();
        return val;
    }

    cham_migratable_task_t* find_and_erase(TYPE_TASK_ID task_id) {
        cham_migratable_task_t* val = nullptr;
        this->m.lock();
        std::unordered_map<TYPE_TASK_ID ,cham_migratable_task_t*>::const_iterator got = this->task_map.find(task_id);
        if(got != this->task_map.end()) {
            val = got->second;
            this->task_map.erase(task_id);
            this->map_size--;
        }
        this->m.unlock();
        return val;
    }
};

class thread_safe_task_list_t {
    private:
    
    std::list<cham_migratable_task_t*> task_list;
    std::mutex m;
    // size_t list_size = 0;
    std::atomic<size_t> list_size;
    
    // duplicate to avoid contention on single atomic from comm thread and worker threads
    std::atomic<size_t> dup_list_size;

    public:

    thread_safe_task_list_t() { list_size = 0; dup_list_size = 0; }

    size_t dup_size() {
        return this->dup_list_size.load();
    }

    size_t size() {
        return this->list_size.load();
    }

    bool empty() {
        return this->list_size <= 0;
    }

    void push_back(cham_migratable_task_t* task) {
        this->m.lock();
        this->task_list.push_back(task);
        this->list_size++;
        this->dup_list_size++;
        this->m.unlock();
    }

    void remove(cham_migratable_task_t* task) {
        this->m.lock();
        this->task_list.remove(task);
        this->list_size--;
        this->dup_list_size--;
        this->m.unlock();
    }

    cham_migratable_task_t* pop_front() {
        if(this->empty())
            return nullptr;

        cham_migratable_task_t* ret_val = nullptr;

        this->m.lock();
        if(!this->empty()) {
            this->list_size--;
            this->dup_list_size--;
            ret_val = this->task_list.front();
            this->task_list.pop_front();
        }
        this->m.unlock();
        return ret_val;
    }

    cham_migratable_task_t* pop_back() {
        if(this->empty())
            return nullptr;

        cham_migratable_task_t* ret_val = nullptr;

        this->m.lock();
        if(!this->empty()) {
            this->list_size--;
            this->dup_list_size--;
            ret_val = this->task_list.back();
            this->task_list.pop_back();
        }
        this->m.unlock();
        return ret_val;
    }

    cham_migratable_task_t* back() {
        if(this->empty())
            return nullptr;
        cham_migratable_task_t* ret_val = nullptr;

        this->m.lock();
        if(!this->empty()) {
            ret_val = this->task_list.back();
        }
        this->m.unlock();
        return ret_val;
    }

    cham_migratable_task_t* front() {
        if(this->empty())
            return nullptr;
        cham_migratable_task_t* ret_val = nullptr;

        this->m.lock();
        if(!this->empty()) {
            ret_val = this->task_list.front();
        }
        this->m.unlock();
        return ret_val;
    }

    cham_migratable_task_t* pop_task_by_id(TYPE_TASK_ID task_id) {
        if(this->empty())
            return nullptr;

        cham_migratable_task_t* ret_val = nullptr;
        this->m.lock();
        if(!this->empty()) {
            for (std::list<cham_migratable_task_t*>::iterator it=this->task_list.begin(); it!=this->task_list.end(); ++it) {
                if((*it)->task_id == task_id)
                {
                    this->list_size--;
                    this->dup_list_size--;
                    ret_val = *it;
                    this->task_list.remove(ret_val);
                    break;
                }
            }
        }
        this->m.unlock();
        return ret_val;
    }

    TYPE_TASK_ID* get_task_ids(int32_t* num_ids) {
        this->m.lock();
        TYPE_TASK_ID *vec = (TYPE_TASK_ID *) malloc(this->task_list.size() * sizeof(TYPE_TASK_ID));
        *num_ids = this->task_list.size();
        size_t count = 0;
        for (std::list<cham_migratable_task_t*>::iterator it=this->task_list.begin(); it!=this->task_list.end(); ++it) {
            vec[count] = (*it)->task_id;
            count++;
        }
        this->m.unlock();
        return vec;
    }
};

template<class T>
class thread_safe_list_t {
    private:
    
    std::list<T> list;
    std::mutex m;
    std::atomic<size_t> list_size;

    public:

    thread_safe_list_t() { list_size = 0; }

    size_t size() {
        return this->list_size.load();
    }

    bool empty() {
        return this->list_size <= 0;
    }

    void push_back(T entry) {
        this->m.lock();
        this->list.push_back(entry);
        this->list_size++;
        this->m.unlock();
    }

    void remove(T entry) {
        this->m.lock();
        this->list.remove(entry);
        this->list_size--;
        this->m.unlock();
    } 

    T pop_front() {
        if(this->empty())
            return NULL;

        T ret_val;

        this->m.lock();
        if(!this->empty()) {
            this->list_size--;
            ret_val = this->list.front();
            this->list.pop_front();
        }
        this->m.unlock();
        return ret_val;
    }

    bool find(T entry) {
        bool found = (std::find(this->list.begin(), this->list.end(), entry) != this->list.end());
        return found;
    }
};
#pragma endregion

#pragma region Variables
extern int chameleon_comm_rank;
extern int chameleon_comm_size;

// atomic counter for task ids
extern std::atomic<TYPE_TASK_ID> _task_id_counter;

// atomic counter for thread ids
extern std::atomic<int32_t> _thread_counter;
extern __thread int __ch_gtid;
extern int32_t __ch_get_gtid();

extern std::mutex _mtx_relp;

extern ch_thread_data_t*    __thread_data;
extern ch_rank_data_t       __rank_data;

extern std::atomic<int> _tracing_enabled;
extern std::atomic<int> _num_sync_cycle;

#ifdef CHAM_DEBUG
extern std::atomic<long> mem_allocated;
#endif

// ============================================================ 
// config values defined through environment variables
// ============================================================
// general settings for migration
extern std::atomic<double> MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION;
extern std::atomic<double> MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE;
extern std::atomic<int> TAG_NBITS_TASK_ID;
extern std::atomic<int> TAG_MAX_TASK_ID;

// settings to manipulate default migration strategy
extern std::atomic<double> MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION;
extern std::atomic<double> MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION;
extern std::atomic<double> PERCENTAGE_DIFF_TASKS_TO_MIGRATE;
extern std::atomic<int> OMP_NUM_THREADS_VAR;

// settings to manipulate replication strategy
extern std::atomic<double> MAX_PERCENTAGE_REPLICATED_TASKS;

// settings to enable / disable tracing only for specific range of synchronization cycles
extern std::atomic<int> ENABLE_TRACE_FROM_SYNC_CYCLE;
extern std::atomic<int> ENABLE_TRACE_TO_SYNC_CYCLE;
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

static void free_migratable_task(cham_migratable_task_t *task, bool is_remote_task = false) {
    if(task) {
        if(task->task_annotations) {
            task->task_annotations->free_annotations();
            delete task->task_annotations;
            task->task_annotations = nullptr;
        }
        if(is_remote_task) {
            for(int i = 0; i < task->arg_num; i++) {
                int64_t tmp_type    = task->arg_types[i];
                int is_lit          = tmp_type & CHAM_OMP_TGT_MAPTYPE_LITERAL;
                if(!is_lit) {
                    free(task->arg_hst_pointers[i]);
                }
            }
        }
        delete task;
        task = nullptr;    
    }
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

#ifndef VT_BEGIN_CONSTRAINED
#define VT_BEGIN_CONSTRAINED(event_id) if (_tracing_enabled) VT_begin(event_id);
#endif

#ifndef VT_END_W_CONSTRAINED
#define VT_END_W_CONSTRAINED(event_id) if (_tracing_enabled) VT_end(event_id);
#endif

template <class Container>
static void split_string(const std::string& str, Container& cont, char delim = ' ')
{
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delim)) {
        cont.push_back(token);
    }
}

static void load_config_values() {
    char *tmp = nullptr;
    tmp = nullptr;
    tmp = std::getenv("OMP_NUM_THREADS");
    if(tmp) {
        OMP_NUM_THREADS_VAR = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION");
    if(tmp) {
        MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION");
    if(tmp) {
        MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION");
    if(tmp) {
        MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE");
    if(tmp) {
        MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("PERCENTAGE_DIFF_TASKS_TO_MIGRATE");
    if(tmp) {
        PERCENTAGE_DIFF_TASKS_TO_MIGRATE = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("MAX_PERCENTAGE_REPLICATED_TASKS");
    if(tmp) {
        MAX_PERCENTAGE_REPLICATED_TASKS = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("ENABLE_TRACE_FROM_SYNC_CYCLE");
    if(tmp) {
        ENABLE_TRACE_FROM_SYNC_CYCLE = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("ENABLE_TRACE_TO_SYNC_CYCLE");
    if(tmp) {
        ENABLE_TRACE_TO_SYNC_CYCLE = std::atof(tmp);
    }

    tmp = nullptr;
    tmp = std::getenv("TAG_NBITS_TASK_ID");
    if(tmp) {
        TAG_NBITS_TASK_ID = std::atof(tmp);
    }
    TAG_MAX_TASK_ID = ((int)pow(2.0, (double)TAG_NBITS_TASK_ID.load()))-1;
}

static void print_config_values() {
    RELP("OMP_NUM_THREADS=%d\n", OMP_NUM_THREADS_VAR.load());
    RELP("MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION=%f\n", MIN_ABS_LOAD_IMBALANCE_BEFORE_MIGRATION.load());
    RELP("MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION=%f\n", MIN_REL_LOAD_IMBALANCE_BEFORE_MIGRATION.load());
    RELP("TAG_NBITS_TASK_ID=%d\n", TAG_NBITS_TASK_ID.load());
    RELP("MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION=%f\n", MIN_LOCAL_TASKS_IN_QUEUE_BEFORE_MIGRATION.load());
    RELP("MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE=%f\n", MAX_TASKS_PER_RANK_TO_MIGRATE_AT_ONCE.load());
    RELP("PERCENTAGE_DIFF_TASKS_TO_MIGRATE=%f\n", PERCENTAGE_DIFF_TASKS_TO_MIGRATE.load());
    RELP("ENABLE_TRACE_FROM_SYNC_CYCLE=%d\n", ENABLE_TRACE_FROM_SYNC_CYCLE.load());
    RELP("ENABLE_TRACE_TO_SYNC_CYCLE=%d\n", ENABLE_TRACE_TO_SYNC_CYCLE.load());
    RELP("MAX_PERCENTAGE_REPLICATED_TASKS=%d\n", MAX_PERCENTAGE_REPLICATED_TASKS.load());
}
#pragma endregion

#endif
