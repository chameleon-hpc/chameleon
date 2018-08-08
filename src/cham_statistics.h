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

#ifdef __cplusplus
extern "C" {
#endif

void cham_stats_init_stats();
void cham_stats_print_stats();

#ifdef __cplusplus
}
#endif
#endif
