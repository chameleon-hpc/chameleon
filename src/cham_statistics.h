#ifndef _CHAMELEON_STATS_H_
#define _CHAMELEON_STATS_H_

#include "chameleon.h"

#ifndef CHAM_STATS_RECORD
#define CHAM_STATS_RECORD 1
#endif

#ifndef CHAM_STATS_PRINT
#define CHAM_STATS_PRINT 1
#endif

extern std::mutex _mtx_num_executed_tasks_local;
extern int _num_executed_tasks_local;

extern std::mutex _mtx_num_executed_tasks_stolen;
extern int _num_executed_tasks_stolen;

extern std::mutex _mtx_num_tasks_offloaded;
extern int _num_tasks_offloaded;

extern std::mutex _mtx_time_task_execution_local;
extern double _time_task_execution_local_sum;
extern int _time_task_execution_local_count;

extern std::mutex _mtx_time_task_execution_stolen;
extern double _time_task_execution_stolen_sum;
extern int _time_task_execution_stolen_count;

extern std::mutex _mtx_time_comm_send_task;
extern double _time_comm_send_task_sum;
extern int _time_comm_send_task_count;

extern std::mutex _mtx_time_comm_recv_task;
extern double _time_comm_recv_task_sum;
extern int _time_comm_recv_task_count;

extern std::mutex _mtx_time_comm_back_send;
extern double _time_comm_back_send_sum;
extern int _time_comm_back_send_count;

extern std::mutex _mtx_time_comm_back_recv;
extern double _time_comm_back_recv_sum;
extern int _time_comm_back_recv_count;

extern std::mutex _mtx_time_encode;
extern double _time_encode_sum;
extern int _time_encode_count;

extern std::mutex _mtx_time_decode;
extern double _time_decode_sum;
extern int _time_decode_count;

#ifdef __cplusplus
extern "C" {
#endif

void cham_stats_init_stats();
void cham_stats_print_stats();

#ifdef __cplusplus
}
#endif
#endif
