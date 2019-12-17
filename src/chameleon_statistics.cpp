#include "chameleon.h"
#include "chameleon_common.h"
#include "chameleon_statistics.h"
#include <float.h>
#include <stdio.h>

MinMaxAvgStats::MinMaxAvgStats(std::string name, std::string unit) {
    stat_name = name;
    unit_str = unit;
    reset();
}

void MinMaxAvgStats::reset() {
    count = 0;
    val_sum = 0.0;
    val_min = DBL_MAX;
    val_max = DBL_MIN;
    val_avg = 0.0;    
}

void MinMaxAvgStats::add_stat_value(double val) {
    if(val > val_max) {
        val_max = val;
    }
    if(val < val_min) {
        val_min = val;
    }

    // add up
    val_sum = val_sum + val;

    if(count == 0)
    {
        val_avg = val;
        count++;
    } else {
        // calculate avg throughput
        double cur_num = (double) count.load();
        val_avg = (val_avg.load() * cur_num + (double) val) / (cur_num+1);
        count++;
    }
}

void MinMaxAvgStats::print_stats(FILE *cur_file) {
    std::stringstream ss; 
    ss.str(""); ss.clear(); ss << stat_name << "_sum (" << unit_str << ")";
    fprintf(cur_file, "Stats R#%d:\t%s\t%lf\n", chameleon_comm_rank, ss.str().c_str(), val_sum.load());
    ss.str(""); ss.clear(); ss << stat_name << "_count";
    fprintf(cur_file, "Stats R#%d:\t%s\t%ld\n", chameleon_comm_rank, ss.str().c_str(), count.load());
    ss.str(""); ss.clear(); ss << stat_name << "_min (" << unit_str << ")";
    cham_stats_print_stats_w_mean(cur_file, ss.str(), val_min.load(), 1);
    ss.str(""); ss.clear(); ss << stat_name << "_max (" << unit_str << ")";
    cham_stats_print_stats_w_mean(cur_file, ss.str(), val_max.load(), 1);
    ss.str(""); ss.clear(); ss << stat_name << "_avg (" << unit_str << ")";
    cham_stats_print_stats_w_mean(cur_file, ss.str(), val_avg.load(), 1);
}

std::atomic<int>     _num_printed_sync_cycles(0);

std::atomic<int>     _num_executed_tasks_local(0);
std::atomic<int>     _num_executed_tasks_stolen(0);
std::atomic<int>     _num_executed_tasks_replicated_local(0);
std::atomic<int>     _num_executed_tasks_replicated_remote(0);
std::atomic<int>     _num_tasks_canceled(0);
std::atomic<int>     _num_tasks_offloaded(0);
std::atomic<int>     _num_migration_decision_performed(0);
std::atomic<int>     _num_migration_done(0);
std::atomic<int>     _num_load_exchanges_performed(0);
std::atomic<int>     _num_slow_communication_operations(0);

std::atomic<double>  _time_data_submit_sum(0.0);
std::atomic<int>     _time_data_submit_count(0);

std::atomic<double>  _time_task_execution_local_sum(0.0);
std::atomic<int>     _time_task_execution_local_count(0);

std::atomic<double>  _time_task_execution_stolen_sum(0.0);
std::atomic<int>     _time_task_execution_stolen_count(0);

std::atomic<double>  _time_task_execution_replicated_sum(0.0);
std::atomic<int>     _time_task_execution_replicated_count(0);

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
std::atomic<int>     _time_commthread_active_count(0);

std::atomic<double>  _time_communication_ongoing_sum(0.0);

MinMaxAvgStats       _stats_bytes_send_per_message("_bytes_send_per_message", "Bytes");
MinMaxAvgStats       _stats_bytes_recv_per_message("_bytes_recv_per_message", "Bytes");
MinMaxAvgStats       _stats_time_comm_send("_time_comm_send", "s, not reliable");
MinMaxAvgStats       _stats_time_comm_recv("_time_comm_recv", "s, not reliable");
MinMaxAvgStats       _stats_throughput_send("_throughput_send", "MB/s, not reliable");
MinMaxAvgStats       _stats_throughput_recv("_throughput_recv", "MB/s, not reliable");

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
    _num_executed_tasks_replicated_local = 0;
    _num_executed_tasks_replicated_remote = 0;
    _num_tasks_offloaded = 0;
    _num_tasks_canceled = 0;
    _num_migration_decision_performed = 0;
    _num_migration_done = 0;
    _num_load_exchanges_performed = 0;
    _num_slow_communication_operations = 0;
  
    _time_task_execution_local_sum = 0.0;
    _time_task_execution_local_count = 0;

    _time_task_execution_stolen_sum = 0.0;
    _time_task_execution_stolen_count = 0;

    _time_task_execution_replicated_sum = 0.0;
    _time_task_execution_replicated_count = 0;

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

    _time_communication_ongoing_sum = 0.0;

    _stats_bytes_send_per_message.reset();
    _stats_throughput_send.reset();
    _stats_bytes_recv_per_message.reset();
    _stats_throughput_recv.reset();
    _stats_time_comm_send.reset();
    _stats_time_comm_recv.reset();
}

void cham_stats_print_stats_w_mean(FILE *cur_file, std::string name, double sum, int count, bool cummulative) {
    std::string prefix = "Stats";
    if(cummulative)
        prefix = "Cumulative Stats";    

    if(count <= 0) {
        fprintf(cur_file, "%s R#%d:\t%s\tsum=\t%.10f\tcount=\t%d\tmean=\t%d\n", prefix.c_str(), chameleon_comm_rank, name.c_str(), sum, count, 0);
    } else {
        fprintf(cur_file, "%s R#%d:\t%s\tsum=\t%.10f\tcount=\t%d\tmean=\t%.10f\n", prefix.c_str(), chameleon_comm_rank, name.c_str(), sum, count, (sum / (double)count));
    }
}

void cham_stats_print_communication_stats(FILE *cur_file, std::string name, double time_avg, double nbytes) {
    fprintf(cur_file, "Stats R#%d:\t%s\teffective throughput=\t%.3f MB/s\n", chameleon_comm_rank, name.c_str(), nbytes/(1e06*time_avg));
}

void cham_stats_print_stats() {
    _mtx_relp.lock();

    FILE *cur_file = stderr;
    char *file_prefix = CHAMELEON_STATS_FILE_PREFIX.load();
    char tmp_file_name[255];

    if(file_prefix) {
        // initial string
        strcpy(tmp_file_name, "");
        // append
        strcat(tmp_file_name, file_prefix);
        strcat(tmp_file_name, "_R");
        strcat(tmp_file_name, std::to_string(chameleon_comm_rank).c_str());
        cur_file = fopen(tmp_file_name, "a");
    }

    #if CHAM_STATS_PER_SYNC_INTERVAL
    fprintf(cur_file, "Stats R#%d:\t===== Printing stats for sync cycle\t%d\t=====\n", chameleon_comm_rank, _num_printed_sync_cycles.load());
    _num_printed_sync_cycles++;
    #endif

    fprintf(cur_file, "Stats R#%d:\t_num_overall_ranks\t%d\n", chameleon_comm_rank, chameleon_comm_size);
    fprintf(cur_file, "Stats R#%d:\t_num_executed_tasks_local\t%d\n", chameleon_comm_rank, _num_executed_tasks_local.load());
    fprintf(cur_file, "Stats R#%d:\t_num_executed_tasks_stolen\t%d\n", chameleon_comm_rank, _num_executed_tasks_stolen.load());
    fprintf(cur_file, "Stats R#%d:\t_num_executed_tasks_replicated_local\t%d\n", chameleon_comm_rank, _num_executed_tasks_replicated_local.load());
    fprintf(cur_file, "Stats R#%d:\t_num_executed_tasks_replicated_remote\t%d\n", chameleon_comm_rank, _num_executed_tasks_replicated_remote.load());
    fprintf(cur_file, "Stats R#%d:\t_num_executed_tasks_overall\t%d\n", chameleon_comm_rank, (_num_executed_tasks_replicated_local.load() + _num_executed_tasks_replicated_remote.load()  + _num_executed_tasks_local.load() + _num_executed_tasks_stolen.load()));
    fprintf(cur_file, "Stats R#%d:\t_num_tasks_offloaded\t%d\n", chameleon_comm_rank, _num_tasks_offloaded.load());
    fprintf(cur_file, "Stats R#%d:\t_num_tasks_canceled\t%d\n", chameleon_comm_rank, _num_tasks_canceled.load());
    fprintf(cur_file, "Stats R#%d:\t_num_migration_decision_performed\t%d\n", chameleon_comm_rank, _num_migration_decision_performed.load());
    fprintf(cur_file, "Stats R#%d:\t_num_migration_done\t%d\n", chameleon_comm_rank, _num_migration_done.load());
    fprintf(cur_file, "Stats R#%d:\t_num_load_exchanges_performed\t%d\n", chameleon_comm_rank, _num_load_exchanges_performed.load());
    fprintf(cur_file, "Stats R#%d:\t_num_slow_communication_operations\t%d\n", chameleon_comm_rank, _num_slow_communication_operations.load());

    cham_stats_print_stats_w_mean(cur_file, "_time_task_execution_local_sum", _time_task_execution_local_sum, _time_task_execution_local_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_task_execution_stolen_sum", _time_task_execution_stolen_sum, _time_task_execution_stolen_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_task_execution_replicated_sum", _time_task_execution_replicated_sum, _time_task_execution_replicated_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_task_execution_overall_sum", (_time_task_execution_replicated_sum+_time_task_execution_local_sum+_time_task_execution_stolen_sum), _time_task_execution_replicated_count+_time_task_execution_local_count+_time_task_execution_stolen_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_encode_sum", _time_encode_sum, _time_encode_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_decode_sum", _time_decode_sum, _time_decode_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_between_load_exchange_sum", _time_between_load_exchange_sum, _time_between_load_exchange_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_between_allgather_and_exchange_sum", _time_between_allgather_and_exchange_sum, _time_between_allgather_and_exchange_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_taskwait_sum", _time_taskwait_sum, _time_taskwait_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_taskwait_idling_sum", _time_taskwait_sum-(_time_task_execution_replicated_sum+_time_task_execution_local_sum+_time_task_execution_stolen_sum), _time_taskwait_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_commthread_active_sum", _time_commthread_active_sum, _time_commthread_active_count);
    cham_stats_print_stats_w_mean(cur_file, "_time_communication_ongoing_sum", _time_communication_ongoing_sum, 1);
    cham_stats_print_stats_w_mean(cur_file, "_time_data_submit_sum", _time_data_submit_sum, _time_data_submit_count);
#if CHAMELEON_TOOL_SUPPORT
    cham_stats_print_stats_w_mean(cur_file, "_time_tool_get_thread_data_sum", _time_tool_get_thread_data_sum, _time_tool_get_thread_data_count, true);
#endif

    _stats_throughput_send.print_stats(cur_file);
    _stats_bytes_send_per_message.print_stats(cur_file);
    _stats_throughput_recv.print_stats(cur_file);
    _stats_bytes_recv_per_message.print_stats(cur_file);
    _stats_time_comm_send.print_stats(cur_file);
    _stats_time_comm_recv.print_stats(cur_file);

    cham_stats_print_communication_stats(cur_file, "total", _time_communication_ongoing_sum, _stats_bytes_send_per_message.val_sum.load() + _stats_bytes_recv_per_message.val_sum.load());
    fprintf(cur_file, "Stats R#%d:\ttask_migration_rate (tasks/s)\t%f\n", chameleon_comm_rank, (double)_num_tasks_offloaded.load() / _time_communication_ongoing_sum.load());
    fprintf(cur_file, "Stats R#%d:\ttask_processing_rate (tasks/s)\t%f\n", chameleon_comm_rank, (double)(_num_executed_tasks_replicated.load() + _num_executed_tasks_local.load() + _num_executed_tasks_stolen.load()) / (_time_taskwait_sum.load() / (double)_time_taskwait_count.load()));

    if(file_prefix) {
        fclose(cur_file);
    }

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
    if(!std::isinf(cur_throughput_mb_s)) {
        // RELP("Send data with n_bytes:\t%d\telapsed_time:\t%.6f sec\tthroughput:\t%.4f MB/s\n", sum_byes, elapsed_sec, cur_throughput_mb_s);
        _stats_throughput_send.add_stat_value(cur_throughput_mb_s);
        _stats_time_comm_send.add_stat_value(elapsed_sec);
    }
}

void add_throughput_recv(double elapsed_sec, int sum_byes) {
    double cur_throughput_mb_s = (double)sum_byes / (elapsed_sec * 1e06);
    if(!std::isinf(cur_throughput_mb_s)) {
        // RELP("Recv data with n_bytes:\t%d\telapsed_time:\t%.6f sec\tthroughput:\t%.4f MB/s\n", sum_byes, elapsed_sec, cur_throughput_mb_s);
        _stats_throughput_recv.add_stat_value(cur_throughput_mb_s);
        _stats_time_comm_recv.add_stat_value(elapsed_sec);
    }
}

#ifdef __cplusplus
}
#endif
