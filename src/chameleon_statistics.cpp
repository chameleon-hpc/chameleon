#include "chameleon.h"
#include "chameleon_common.h"
#include "chameleon_statistics.h"
#include <float.h>

std::atomic<int>     _num_executed_tasks_local(0);
std::atomic<int>     _num_executed_tasks_stolen(0);
std::atomic<int>     _num_executed_tasks_replicated(0);
std::atomic<int>     _num_tasks_canceled(0);
std::atomic<int>     _num_tasks_offloaded(0);
std::atomic<int>     _num_migration_decision_performed(0);
std::atomic<int>     _num_migration_done(0);
std::atomic<int>     _num_load_exchanges_performed(0);
std::atomic<int>     _num_slow_communication_operations(0);
std::atomic<int>     _num_bytes_sent(0);
std::atomic<int>     _num_bytes_received(0);

std::atomic<double>  _time_data_submit_sum(0.0);
std::atomic<int>     _time_data_submit_count(0);

std::atomic<double>  _time_task_execution_local_sum(0.0);
std::atomic<int>     _time_task_execution_local_count(0);

std::atomic<double>  _time_task_execution_stolen_sum(0.0);
std::atomic<int>     _time_task_execution_stolen_count(0);

std::atomic<double>  _time_task_execution_replicated_sum(0.0);
std::atomic<int>     _time_task_execution_replicated_count(0);

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

std::atomic<double>  _time_between_load_exchange_sum(0.0);
std::atomic<int64_t> _time_between_load_exchange_count(0);

std::atomic<double>  _time_between_allgather_and_exchange_sum(0.0);
std::atomic<int64_t> _time_between_allgather_and_exchange_count(0);

std::atomic<double>  _time_taskwait_sum(0.0);
std::atomic<int>     _time_taskwait_count(0);

std::atomic<double>  _time_commthread_active_sum(0.0);
std::atomic<int>     _time_commthread_active_count(0.0);

std::atomic<double>  _throughput_send_min(DBL_MAX);
std::atomic<double>  _throughput_send_max(DBL_MIN);
std::atomic<double>  _throughput_send_avg(0.0);
std::atomic<int>     _throughput_send_num(0);

std::atomic<double>  _throughput_recv_min(DBL_MAX);
std::atomic<double>  _throughput_recv_max(DBL_MIN);
std::atomic<double>  _throughput_recv_avg(0.0);
std::atomic<int>     _throughput_recv_num(0);

#if CHAMELEON_TOOL_SUPPORT
std::atomic<double>  _time_tool_get_thread_data_sum(0.0);
std::atomic<int>     _time_tool_get_thread_data_count(0);
#endif

#ifdef __cplusplus
extern "C" {
#endif

void cham_stats_reset_for_sync_cycle() {
    _num_executed_tasks_local = 0;
    _num_executed_tasks_stolen = 0;
    _num_executed_tasks_replicated = 0;
    _num_tasks_offloaded = 0;
    _num_tasks_canceled = 0;
    _num_migration_decision_performed = 0;
    _num_migration_done = 0;
    _num_load_exchanges_performed = 0;
    _num_slow_communication_operations = 0;
    _num_bytes_sent = 0;
    _num_bytes_received = 0;
  
    _time_task_execution_local_sum = 0.0;
    _time_task_execution_local_count = 0;

    _time_task_execution_stolen_sum = 0.0;
    _time_task_execution_stolen_count = 0;

    _time_task_execution_replicated_sum = 0.0;
    _time_task_execution_replicated_count = 0;

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

    _time_between_load_exchange_sum = 0.0;
    _time_between_load_exchange_count = 0;

    _time_between_allgather_and_exchange_sum = 0.0;
    _time_between_allgather_and_exchange_count = 0;

    _time_taskwait_sum = 0.0;
    _time_taskwait_count = 0;

    _time_commthread_active_sum = 0.0;
    _time_commthread_active_count = 0;

    _throughput_send_max = DBL_MIN;
    _throughput_send_min = DBL_MAX;
    _throughput_send_avg = 0.0;
    _throughput_send_num = 0;

    _throughput_recv_max = DBL_MIN;
    _throughput_recv_min = DBL_MAX;
    _throughput_recv_avg = 0.0;
    _throughput_recv_num = 0;
}

void cham_stats_print_stats_w_mean(std::string name, double sum, int count, bool cummulative = false) {
    std::string prefix = "Stats";
    if(cummulative)
        prefix = "Cumulative Stats";

    if(count <= 0) {
        fprintf(stderr, "%s R#%d:\t%s\tsum=\t%.10f\tcount=\t%d\tmean=\t%d\n", prefix.c_str(), chameleon_comm_rank, name.c_str(), sum, count, 0);
    } else {
        fprintf(stderr, "%s R#%d:\t%s\tsum=\t%.10f\tcount=\t%d\tmean=\t%.10f\n", prefix.c_str(), chameleon_comm_rank, name.c_str(), sum, count, (sum / (double)count));
    }
}

void cham_stats_print_communication_stats(std::string name, double time_avg, int nbytes) {
    std::string prefix = "Stats";
 
    fprintf(stderr, "%s R#%d:\t%s\teffective throughput=\t%.3f MB/s\n", prefix.c_str(), chameleon_comm_rank, name.c_str(), nbytes/(1e06*time_avg));
}

void cham_stats_print_stats() {
    _mtx_relp.lock();
    fprintf(stderr, "Stats R#%d:\t_num_overall_ranks\t%d\n", chameleon_comm_rank, chameleon_comm_size);
    fprintf(stderr, "Stats R#%d:\t_num_executed_tasks_local\t%d\n", chameleon_comm_rank, _num_executed_tasks_local.load());
    fprintf(stderr, "Stats R#%d:\t_num_executed_tasks_stolen\t%d\n", chameleon_comm_rank, _num_executed_tasks_stolen.load());
    fprintf(stderr, "Stats R#%d:\t_num_executed_tasks_replicated\t%d\n", chameleon_comm_rank, _num_executed_tasks_replicated.load());
    fprintf(stderr, "Stats R#%d:\t_num_executed_tasks_overall\t%d\n", chameleon_comm_rank, (_num_executed_tasks_replicated.load() + _num_executed_tasks_local.load() + _num_executed_tasks_stolen.load()));
    fprintf(stderr, "Stats R#%d:\t_num_tasks_offloaded\t%d\n", chameleon_comm_rank, _num_tasks_offloaded.load());
    fprintf(stderr, "Stats R#%d:\t_num_tasks_canceled\t%d\n", chameleon_comm_rank, _num_tasks_canceled.load());
    fprintf(stderr, "Stats R#%d:\t_num_migration_decision_performed\t%d\n", chameleon_comm_rank, _num_migration_decision_performed.load());
    fprintf(stderr, "Stats R#%d:\t_num_migration_done\t%d\n", chameleon_comm_rank, _num_migration_done.load());
    fprintf(stderr, "Stats R#%d:\t_num_load_exchanges_performed\t%d\n", chameleon_comm_rank, _num_load_exchanges_performed.load());
    fprintf(stderr, "Stats R#%d:\t_num_bytes_sent\t%d\n", chameleon_comm_rank, _num_bytes_sent.load());
    fprintf(stderr, "Stats R#%d:\t_num_bytes_received\t%d\n", chameleon_comm_rank, _num_bytes_received.load());
    fprintf(stderr, "Stats R#%d:\t_num_slow_communication_operations\t%d\n", chameleon_comm_rank, _num_slow_communication_operations.load());

    cham_stats_print_stats_w_mean("_time_task_execution_local_sum", _time_task_execution_local_sum, _time_task_execution_local_count);
    cham_stats_print_stats_w_mean("_time_task_execution_stolen_sum", _time_task_execution_stolen_sum, _time_task_execution_stolen_count);
    cham_stats_print_stats_w_mean("_time_task_execution_replicated_sum", _time_task_execution_replicated_sum, _time_task_execution_replicated_count);
    cham_stats_print_stats_w_mean("_time_task_execution_overall_sum", (_time_task_execution_replicated_sum+_time_task_execution_local_sum+_time_task_execution_stolen_sum), _time_task_execution_replicated_count+_time_task_execution_local_count+_time_task_execution_stolen_count);
    cham_stats_print_stats_w_mean("_time_comm_send_task_sum", _time_comm_send_task_sum, _time_comm_send_task_count);
    cham_stats_print_stats_w_mean("_time_comm_recv_task_sum", _time_comm_recv_task_sum, _time_comm_recv_task_count);
    cham_stats_print_stats_w_mean("_time_comm_back_send_sum", _time_comm_back_send_sum, _time_comm_back_send_count);
    cham_stats_print_stats_w_mean("_time_comm_back_recv_sum", _time_comm_back_recv_sum, _time_comm_back_recv_count);
    cham_stats_print_stats_w_mean("_time_encode_sum", _time_encode_sum, _time_encode_count);
    cham_stats_print_stats_w_mean("_time_decode_sum", _time_decode_sum, _time_decode_count);
    cham_stats_print_stats_w_mean("_time_between_load_exchange_sum", _time_between_load_exchange_sum, _time_between_load_exchange_count);
    cham_stats_print_stats_w_mean("_time_between_allgather_and_exchange_sum", _time_between_allgather_and_exchange_sum, _time_between_allgather_and_exchange_count);
    cham_stats_print_stats_w_mean("_time_taskwait_sum", _time_taskwait_sum, _time_taskwait_count);
    cham_stats_print_stats_w_mean("_time_taskwait_idling_sum", _time_taskwait_sum-(_time_task_execution_replicated_sum+_time_task_execution_local_sum+_time_task_execution_stolen_sum), _time_taskwait_count);
    cham_stats_print_stats_w_mean("_time_commthread_active_sum", _time_commthread_active_sum, _time_commthread_active_count);
    cham_stats_print_stats_w_mean("_time_data_submit_sum", _time_data_submit_sum, _time_data_submit_count, true);
#if CHAMELEON_TOOL_SUPPORT
    cham_stats_print_stats_w_mean("_time_tool_get_thread_data_sum", _time_tool_get_thread_data_sum, _time_tool_get_thread_data_count, true);
#endif

    cham_stats_print_stats_w_mean("_throughput_send_min (MB/s, not reliable)", _throughput_send_min, 1);
    cham_stats_print_stats_w_mean("_throughput_send_max (MB/s, not reliable)", _throughput_send_max, 1);
    cham_stats_print_stats_w_mean("_throughput_send_avg (MB/s, not reliable)", _throughput_send_avg, 1);
    fprintf(stderr, "Stats R#%d:\t_throughput_send_num\t%d\n", chameleon_comm_rank, _throughput_send_num.load());
    cham_stats_print_stats_w_mean("_throughput_recv_min (MB/s, not reliable)", _throughput_recv_min, 1);
    cham_stats_print_stats_w_mean("_throughput_recv_max (MB/s, not reliable)", _throughput_recv_max, 1);
    cham_stats_print_stats_w_mean("_throughput_recv_avg (MB/s, not reliable)", _throughput_recv_avg, 1);
    fprintf(stderr, "Stats R#%d:\t_throughput_recv_num\t%d\n", chameleon_comm_rank, _throughput_recv_num.load());

    cham_stats_print_communication_stats("sending", _time_commthread_active_sum, _num_bytes_sent);
    cham_stats_print_communication_stats("receiving", _time_commthread_active_sum, _num_bytes_received);
    cham_stats_print_communication_stats("total", _time_commthread_active_sum, _num_bytes_sent+_num_bytes_received);
    _mtx_relp.unlock();
}

/*
 * Atomic addition for double
 */
 void atomic_add_dbl(std::atomic<double> &dbl, double d){
      double old = dbl.load(std::memory_order_consume);
      double desired = old + d;
      while (!dbl.compare_exchange_weak(old, desired,
           std::memory_order_release, std::memory_order_consume))
      {
           desired = old + d;
      }
 }

void add_throughput_send(double elapsed_sec, int sum_byes) {
    double cur_throughput_mb_s = (double)sum_byes / (elapsed_sec * 1e06);
    // RELP("Send data with n_bytes:\t%d\telapsed_time:\t%.6f sec\tthroughput:\t%.4f MB/s\n", sum_byes, elapsed_sec, cur_throughput_mb_s);
    if(cur_throughput_mb_s > _throughput_send_max) {
        _throughput_send_max = cur_throughput_mb_s;
    }
    if(cur_throughput_mb_s < _throughput_send_min) {
        _throughput_send_min = cur_throughput_mb_s;
    }

    if(_throughput_send_num == 0)
    {
        _throughput_send_avg = cur_throughput_mb_s;
        _throughput_send_num++;
    } else {
        // calculate avg throughput
        double cur_num = (double) _throughput_send_num.load();
        _throughput_send_avg = (_throughput_send_avg.load() * cur_num + cur_throughput_mb_s) / (cur_num+1);
        _throughput_send_num++;
    }
}

void add_throughput_recv(double elapsed_sec, int sum_byes) {
    double cur_throughput_mb_s = (double)sum_byes / (elapsed_sec * 1e06);
    // RELP("Recv data with n_bytes:\t%d\telapsed_time:\t%.6f sec\tthroughput:\t%.4f MB/s\n", sum_byes, elapsed_sec, cur_throughput_mb_s);
    if(cur_throughput_mb_s > _throughput_recv_max) {
        _throughput_recv_max = cur_throughput_mb_s;
    }
    if(cur_throughput_mb_s < _throughput_recv_min) {
        _throughput_recv_min = cur_throughput_mb_s;
    }

    if(_throughput_recv_num == 0)
    {
        _throughput_recv_avg = cur_throughput_mb_s;
        _throughput_recv_num++;
    } else {
        // calculate avg throughput
        double cur_num = (double) _throughput_recv_num.load();
        _throughput_recv_avg = (_throughput_recv_avg.load() * cur_num + cur_throughput_mb_s) / (cur_num+1);
        _throughput_recv_num++;
    }
}

#ifdef __cplusplus
}
#endif
