/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 * Rebuild a VP9 frame from the NVDEC picture setup, probabilities and frame body.
 * The builder belongs to one NVDEC channel and retains VP9 reference state across jobs. */
#ifndef SNX_VP9_REBUILD_H
#define SNX_VP9_REBUILD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnxVp9Builder SnxVp9Builder;
typedef struct {
    const uint8_t* data; /* valid until the next compose call */
    uint32_t size;
    uint32_t width, height, pitch_y, pitch_c;
    uint32_t layout;
    uint64_t luma, chroma;
    uint8_t previous_visible;
} SnxVp9Frame;

SnxVp9Builder* snx_vp9_new(void);
void snx_vp9_free(SnxVp9Builder* builder);
/* Returns 1 on success. Inputs are the NVDEC picture (0x100 bytes), probability table (0xEA0 bytes), raw frame
 * body (body_size bytes), and channel register file. Rejects malformed sizes and out-of-range values. */
int snx_vp9_compose(SnxVp9Builder* builder, const uint8_t* picture, const uint8_t* probabilities, const uint8_t* body,
                    uint32_t body_size, const uint32_t* regs, SnxVp9Frame* frame);

#ifdef __cplusplus
}
#endif
#endif
