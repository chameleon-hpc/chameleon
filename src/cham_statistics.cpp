#include "cham_statistics.h"
#include "chameleon.h"
#include "chameleon_common.h"

std::atomic<int>     _num_executed_tasks_local(0);
std::atomic<int>     _num_executed_tasks_stolen(0);
std::atomic<int>     _num_tasks_offloaded(0);

std::atomic<double>  _time_task_execution_local_sum(0.0);
std::atomic<int>     _time_task_execution_local_count(0);

std::atomic<double>  _time_task_execution_stolen_sum(0.0);
std::atomic<int>     _time_task_execution_stolen_count(0);

std::atomic<double>  _time_comm_send_task_sum(0.0);
std::atomic<int>     _time_comm_send_task_count(0);

std::atomic<double>  _time_comm_recv_task_sum(0.0);
std::atomic<int>     _time_comm_recv_task_count(0);

std::atomic<double>  _time_comm_back_send_sum(0.0);
std::atomic<int>     _time_comm_back_send_count(0);

std::atomic<double>  _time_comm_back_recv_sum(0.0);
std::atomic<int>     _time_comm_back_recv_count(0);

std::atomic<double>  _time_encode_sum(0.0);
std::atomic<int>     _time_encode_count(0);

std::atomic<double>  _time_decode_sum(0.0);
std::atomic<int>     _time_decode_count(0);

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
        printf("Stats R#%d:\t%s\tsum=\t%.10f\tcount=\t%d\tmean=\t%d\n", chameleon_comm_rank, name.c_str(), sum, count, 0);
    } else {
        printf("Stats R#%d:\t%s\tsum=\t%.10f\tcount=\t%d\tmean=\t%.10f\n", chameleon_comm_rank, name.c_str(), sum, count, (sum / (double)count));
    }
}

void cham_stats_print_stats() {
    printf("Stats R#%d:\t_num_overall_ranks\t%d\n", chameleon_comm_rank, chameleon_comm_size);
    printf("Stats R#%d:\t_num_executed_tasks_local\t%d\n", chameleon_comm_rank, _num_executed_tasks_local.load());
    printf("Stats R#%d:\t_num_executed_tasks_stolen\t%d\n", chameleon_comm_rank, _num_executed_tasks_stolen.load());
    printf("Stats R#%d:\t_num_tasks_offloaded\t%d\n", chameleon_comm_rank, _num_tasks_offloaded.load());

    cham_stats_print_stats_w_mean("_time_task_execution_local_sum", _time_task_execution_local_sum, _time_task_execution_local_count);
    cham_stats_print_stats_w_mean("_time_task_execution_stolen_sum", _time_task_execution_stolen_sum, _time_task_execution_stolen_count);
    cham_stats_print_stats_w_mean("_time_comm_send_task_sum", _time_comm_send_task_sum, _time_comm_send_task_count);
    cham_stats_print_stats_w_mean("_time_comm_recv_task_sum", _time_comm_recv_task_sum, _time_comm_recv_task_count);
    cham_stats_print_stats_w_mean("_time_comm_back_send_sum", _time_comm_back_send_sum, _time_comm_back_send_count);
    cham_stats_print_stats_w_mean("_time_comm_back_recv_sum", _time_comm_back_recv_sum, _time_comm_back_recv_count);
    cham_stats_print_stats_w_mean("_time_encode_sum", _time_encode_sum, _time_encode_count);
    cham_stats_print_stats_w_mean("_time_decode_sum", _time_decode_sum, _time_decode_count);
}

/*
 * Atomic addition for double
 */
 double atomic_add_dbl(std::atomic<double> &dbl, double d){
      double old = dbl.load(std::memory_order_consume);
      double desired = old + d;
      while (!dbl.compare_exchange_weak(old, desired,
           std::memory_order_release, std::memory_order_consume))
      {
           desired = old + d;
      }
      return desired;
 }

#ifdef __cplusplus
}
#endif
