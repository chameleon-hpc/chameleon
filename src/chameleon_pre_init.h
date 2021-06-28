#ifndef _CHAMELEON_PRE_INIT_H_
#define _CHAMELEON_PRE_INIT_H_

#include <chameleon.h>

#ifdef __GNUG__
// remeber original cpuset for the complete process
cpu_set_t proc_mask;
void chameleon_preinit_hook(int argc, char **argv, char **envp) {
    sched_getaffinity(getpid(), sizeof(cpu_set_t), &proc_mask);
}
__attribute__((section(".preinit_array"))) __typeof__(chameleon_preinit_hook) *__preinit = chameleon_preinit_hook;

void chameleon_pre_init() {
    chameleon_set_proc_cpuset(proc_mask);
}
#else /* __GNUG__ */
void chameleon_pre_init();
#endif /* __GNUG__ */
#endif /* _CHAMELEON_PRE_INIT_H_ */
