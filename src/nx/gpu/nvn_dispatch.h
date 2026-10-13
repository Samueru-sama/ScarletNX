/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_DISPATCH_H
#define SNX_NVN_DISPATCH_H
#include "gpu/nvn_draw.h"

typedef struct NvnStorageSeed {
    struct NvnStorageSeed* next;
    u64 address;
    u32 size;
    const u8* guest_host;
    u8 bytes[];
} NvnStorageSeed;

typedef struct {
    u64 sequence, prior_unmodeled_write;
    NvnStageProgram program;
    u32 groups[3], local[3], shared_size;
    u32 embedded_cbuf1_offset;
    NvnBufferRange uniform[NVN_DRAW_UNIFORM_SLOTS];
    u8 uniform0_snapshot[512];
    u16 uniform0_snapshot_size;
    const NvnStorageSeed* uniform_seed[5];
    const NvnUniformUpdate* uniform_updates;
    u8 uniform_updates_complete;
    NvnBufferRange storage[NVN_DRAW_STORAGE_SLOTS];
    const NvnStorageSeed* storage_seed[NVN_DRAW_STORAGE_SLOTS];
    u16 uniform_bound, storage_bound;
    u64 texture[NVN_DRAW_TEXTURE_SLOTS];
    u32 texture_bound;
    NvnDescriptorSnapshot texture0_descriptor;
    NvnSamplerDescriptorSnapshot texture0_sampler;
    u8 texture0_descriptor_valid, texture0_sampler_valid;
    u8 control_known;
} NvnDispatchCommand;

#endif
