#include "cham_statistics.h"

std::mutex _mtx_num_executed_tasks_local;
int _num_executed_tasks_local = 0;

std::mutex _mtx_num_executed_tasks_stolen;
int _num_executed_tasks_stolen = 0;

std::mutex _mtx_num_tasks_offloaded;
int _num_tasks_offloaded = 0;

#ifdef __cplusplus
extern "C" {
#endif

void cham_stats_init_stats() {
    _num_executed_tasks_local = 0;
    _num_executed_tasks_stolen = 0;
    _num_tasks_offloaded = 0;
}

void cham_stats_print_stats() {
    printf("Stats R#%d:\t_num_executed_tasks_local\t%d\n", chameleon_comm_rank, _num_executed_tasks_local);
    printf("Stats R#%d:\t_num_executed_tasks_stolen\t%d\n", chameleon_comm_rank, _num_executed_tasks_stolen);
    printf("Stats R#%d:\t_num_tasks_offloaded\t%d\n", chameleon_comm_rank, _num_tasks_offloaded);
}

#ifdef __cplusplus
}
#endif
