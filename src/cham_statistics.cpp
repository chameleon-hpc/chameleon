#include "cham_statistics.h"

std::mutex _mtx_num_executed_tasks_local;
int _num_executed_tasks_local = 0;

std::mutex _mtx_num_executed_tasks_stolen;
int _num_executed_tasks_stolen = 0;

std::mutex _mtx_num_tasks_offloaded;
int _num_tasks_offloaded = 0;

std::mutex _mtx_time_task_execution_local;
double _time_task_execution_local_sum;
int _time_task_execution_local_count;

std::mutex _mtx_time_task_execution_stolen;
double _time_task_execution_stolen_sum;
int _time_task_execution_stolen_count;

std::mutex _mtx_time_comm_send_task;
double _time_comm_send_task_sum;
int _time_comm_send_task_count;

std::mutex _mtx_time_comm_recv_task;
double _time_comm_recv_task_sum;
int _time_comm_recv_task_count;

std::mutex _mtx_time_comm_back_send;
double _time_comm_back_send_sum;
int _time_comm_back_send_count;

std::mutex _mtx_time_comm_back_recv;
double _time_comm_back_recv_sum;
int _time_comm_back_recv_count;

std::mutex _mtx_time_encode;
double _time_encode_sum;
int _time_encode_count;

std::mutex _mtx_time_decode;
double _time_decode_sum;
int _time_decode_count;

#ifdef __cplusplus
extern "C" {
#endif

void cham_stats_init_stats() {
    _num_executed_tasks_local = 0;
    _num_executed_tasks_stolen = 0;
    _num_tasks_offloaded = 0;

    _time_task_execution_local_sum = 0.0;
    _time_task_execution_local_count = 0;

    _time_task_execution_stolen_sum = 0.0;
    _time_task_execution_stolen_count = 0;

    _time_comm_send_task_sum = 0.0;
    _time_comm_send_task_count = 0;

    _time_comm_recv_task_sum = 0.0;
    _time_comm_recv_task_count = 0;

    _time_comm_back_send_sum = 0.0;
    _time_comm_back_send_count = 0;

    _time_comm_back_recv_sum = 0.0;
    _time_comm_back_recv_count = 0;

    _time_encode_sum = 0.0;
    _time_encode_count = 0;

    _time_decode_sum = 0.0;
    _time_decode_count = 0;
}

void cham_stats_print_stats_w_mean(std::string name, double sum, int count) {
    if(count <= 0) {
        printf("Stats R#%d:\t%s\tsum=\t%f\tcount=\t%d\tmean=\t%d\n", chameleon_comm_rank, name.c_str(), sum, count, 0);
    } else {
        printf("Stats R#%d:\t%s\tsum=\t%f\tcount=\t%d\tmean=\t%f\n", chameleon_comm_rank, name.c_str(), sum, count, (sum / (double)count));
    }
}

void cham_stats_print_stats() {
    printf("Stats R#%d:\t_num_executed_tasks_local\t%d\n", chameleon_comm_rank, _num_executed_tasks_local);
    printf("Stats R#%d:\t_num_executed_tasks_stolen\t%d\n", chameleon_comm_rank, _num_executed_tasks_stolen);
    printf("Stats R#%d:\t_num_tasks_offloaded\t%d\n", chameleon_comm_rank, _num_tasks_offloaded);

    cham_stats_print_stats_w_mean("_time_task_execution_local_sum", _time_task_execution_local_sum, _time_task_execution_local_count);
    cham_stats_print_stats_w_mean("_time_task_execution_stolen_sum", _time_task_execution_stolen_sum, _time_task_execution_stolen_count);
    cham_stats_print_stats_w_mean("_time_comm_send_task_sum", _time_comm_send_task_sum, _time_comm_send_task_count);
    cham_stats_print_stats_w_mean("_time_comm_recv_task_sum", _time_comm_recv_task_sum, _time_comm_recv_task_count);
    cham_stats_print_stats_w_mean("_time_comm_back_send_sum", _time_comm_back_send_sum, _time_comm_back_send_count);
    cham_stats_print_stats_w_mean("_time_comm_back_recv_sum", _time_comm_back_recv_sum, _time_comm_back_recv_count);
    cham_stats_print_stats_w_mean("_time_encode_sum", _time_encode_sum, _time_encode_count);
    cham_stats_print_stats_w_mean("_time_decode_sum", _time_decode_sum, _time_decode_count);
}

#ifdef __cplusplus
}
#endif
