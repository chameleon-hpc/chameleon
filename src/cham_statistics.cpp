#include "cham_statistics.h"
#include "chameleon.h"
#include "chameleon_common.h"

std::atomic<int>     _num_executed_tasks_local(0);
std::atomic<int>     _num_executed_tasks_stolen(0);
std::atomic<int>     _num_executed_tasks_replicated(0);
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

    cham_stats_print_stats_w_mean("_time_data_submit_sum", _time_data_submit_sum, _time_data_submit_count, true);
#if CHAMELEON_TOOL_SUPPORT
    cham_stats_print_stats_w_mean("_time_tool_get_thread_data_sum", _time_tool_get_thread_data_sum, _time_tool_get_thread_data_count, true);
#endif
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

#ifdef __cplusplus
}
#endif
