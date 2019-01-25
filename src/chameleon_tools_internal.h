#ifndef _CHAMELEON_TOOLS_INTERNAL_H_
#define _CHAMELEON_TOOLS_INTERNAL_H_

#include "chameleon_common.h"
#include "chameleon_tools.h"

typedef struct cham_t_callbacks_active_s {
    unsigned int enabled : 1;
    // TODO: add callback pointers here
    cham_t_callback_task_create_t cham_t_callback_task_create = nullptr;

} cham_t_callbacks_active_t;

extern cham_t_callbacks_active_t cham_t_enabled;

#ifdef __cplusplus
extern "C" {
#endif

void cham_t_init(void);

void cham_t_fini(void);

#ifdef __cplusplus
};
#endif

#endif