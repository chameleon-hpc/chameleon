// Load balancing strategies header
#ifndef _CHAMELEON_STRATEGIES_H_
#define _CHAMELEON_STRATEGIES_H_

#include <vector>
#include <stdint.h>

void computeNumTasksToOffload( std::vector<int32_t>& tasksToOffloadPerRank, std::vector<int32_t>& loadInfoRanks );


#endif
