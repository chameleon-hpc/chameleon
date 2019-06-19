#ifndef _CHAMELEON_STATS_H_
#define _CHAMELEON_STATS_H_

#include "chameleon.h"
#include "chameleon_common.h"

#ifndef CHAM_STATS_RECORD
#define CHAM_STATS_RECORD 0
#endif

#ifndef CHAM_STATS_PER_SYNC_INTERVAL
#define CHAM_STATS_PER_SYNC_INTERVAL 0
#endif

#ifndef CHAM_SLOW_COMMUNICATION_THRESHOLD
#define CHAM_SLOW_COMMUNICATION_THRESHOLD 0.001
#endif

#ifndef CHAM_STATS_PRINT
#define CHAM_STATS_PRINT 0
#endif

extern std::atomic<int>     _num_executed_tasks_local;
extern std::atomic<int>     _num_executed_tasks_stolen;
extern std::atomic<int>     _num_executed_tasks_replicated;
extern std::atomic<int>     _num_tasks_offloaded;
extern std::atomic<int>     _num_tasks_canceled;
extern std::atomic<int>     _num_migration_decision_performed;
extern std::atomic<int>     _num_migration_done;
extern std::atomic<int>     _num_load_exchanges_performed;
extern std::atomic<int>     _num_slow_communication_operations;
extern std::atomic<int>     _num_bytes_received;
extern std::atomic<int>     _num_bytes_sent;

extern std::atomic<double>  _time_data_submit_sum;
extern std::atomic<int>     _time_data_submit_count;

extern std::atomic<double>  _time_task_execution_local_sum;
extern std::atomic<int>     _time_task_execution_local_count;

extern std::atomic<double>  _time_task_execution_replicated_sum;
extern std::atomic<int>     _time_task_execution_replicated_count;

extern std::atomic<double>  _time_task_execution_stolen_sum;
extern std::atomic<int>     _time_task_execution_stolen_count;

extern std::atomic<double>  _time_comm_send_task_sum;
extern std::atomic<int>     _time_comm_send_task_count;

extern std::atomic<double>  _time_comm_recv_task_sum;
extern std::atomic<int>     _time_comm_recv_task_count;

extern std::atomic<double>  _time_comm_back_send_sum;
extern std::atomic<int>     _time_comm_back_send_count;

extern std::atomic<double>  _time_comm_back_recv_sum;
extern std::atomic<int>     _time_comm_back_recv_count;

extern std::atomic<double>  _time_encode_sum;
extern std::atomic<int>     _time_encode_count;

extern std::atomic<double>  _time_decode_sum;
extern std::atomic<int>     _time_decode_count;

extern std::atomic<double>  _time_between_allgather_and_exchange_sum;
extern std::atomic<int64_t> _time_between_allgather_and_exchange_count;

extern std::atomic<double>  _time_between_load_exchange_sum;
extern std::atomic<int64_t> _time_between_load_exchange_count;

extern std::atomic<double>  _time_taskwait_sum;
extern std::atomic<int>     _time_taskwait_count;

extern std::atomic<double>  _time_commthread_active_sum;
extern std::atomic<int>     _time_commthread_active_count;

extern std::atomic<double>  _throughput_send_min;
extern std::atomic<double>  _throughput_send_max;
extern std::atomic<double>  _throughput_send_avg;
extern std::atomic<int>     _throughput_send_num;

extern std::atomic<double>  _throughput_recv_min;
extern std::atomic<double>  _throughput_recv_max;
extern std::atomic<double>  _throughput_recv_avg;
extern std::atomic<int>     _throughput_recv_num;

#if CHAMELEON_TOOL_SUPPORT
extern std::atomic<double>  _time_tool_get_thread_data_sum;
extern std::atomic<int>     _time_tool_get_thread_data_count;
#endif

#ifdef __cplusplus
extern "C" {
#endif

void cham_stats_reset_for_sync_cycle();
void cham_stats_print_stats();
void atomic_add_dbl(std::atomic<double> &f, double d);
void add_throughput_send(double elapsed_sec, int sum_byes);
void add_throughput_recv(double elapsed_sec, int sum_byes);

#ifdef __cplusplus
}
#endif
#endif
