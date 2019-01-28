#ifndef _CHAMELEON_COMMON_CPP_
#define _CHAMELEON_COMMON_CPP_

#include "chameleon_common.h"

// atomic counter for task ids
std::atomic<int32_t> _thread_counter(0);
__thread int32_t __ch_gtid = -1;

int32_t __ch_get_gtid() {
    if(__ch_gtid != -1)
        return __ch_gtid;

    __ch_gtid = _thread_counter++;
    return __ch_gtid;
}
#endif