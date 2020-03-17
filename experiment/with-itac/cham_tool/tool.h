#include "chameleon.h"
#include "chameleon_tools.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <inttypes.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>

#define TIMESTAMP(time_) 						\
  do {									\
      struct timespec ts;						\
      clock_gettime(CLOCK_MONOTONIC, &ts);				\
      time_=((double)ts.tv_sec)+(1.0e-9)*((double)ts.tv_nsec);		\
  } while(0)

// class for profiling task
cham_t_task_lis_t tool_task_list;

void chameleon_t_print(char data[])
{
    struct timeval curTime;
    gettimeofday(&curTime, NULL);
    int milli = curTime.tv_usec / 1000;
    int micro_sec = curTime.tv_usec % 1000;
    char buffer [80];
    strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", localtime(&curTime.tv_sec));
    char currentTime[84] = "";
    sprintf(currentTime, "%s.%03d.%03d", buffer, milli, micro_sec);
    printf("[CHAM_T] Timestamp-%s: %s\n", currentTime, data);
}
