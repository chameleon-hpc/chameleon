// Chameleon Header
#ifndef _CHAMELEON_H_
#define _CHAMELEON_H_

#include <cstdint>
#include <stddef.h>
#include <stdint.h>

struct OffloadingDataEntryTy {
    void *tgt_ptr;
    void *hst_ptr;
    int64_t size;
    int32_t ref_count;

    // does it make sense to put these two here? (might be used in multiple target regions)
    int32_t send_back;
    int32_t is_implicit;

    OffloadingDataEntryTy() {
        tgt_ptr = nullptr;
        hst_ptr = nullptr;
        size = 0;
        ref_count = 1;

        send_back = 0;
        is_implicit = 0;
    }

    OffloadingDataEntryTy(void *p_tgt_ptr, void *p_hst_ptr, int64_t p_size) {
        tgt_ptr = p_tgt_ptr;
        hst_ptr = p_hst_ptr;
        size = p_size;
        ref_count = 1;

        send_back = 0;
        is_implicit = 0;
    }
};

#ifdef __cplusplus
extern "C" {
#endif

int32_t chameleon_init();

int32_t chameleon_finalize();

int32_t chameleon_distributed_taskwait();

int32_t chameleon_submit_data(void *tgt_ptr, void *hst_ptr, int64_t size);

int32_t chameleon_submit_implicit_data(void *tgt_ptr);

#ifdef __cplusplus
}
#endif

#endif
