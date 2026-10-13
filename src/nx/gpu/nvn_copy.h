/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_COPY_H
#define SNX_NVN_COPY_H
#include "gpu/nvn_texture.h"

typedef struct {
    u64 sequence;
    u64 unknown_marker, prior_unmodeled_write;
    NvnTextureTarget target;
    NvnTextureTarget source_target;
    u64 source;
    i64 row_stride, image_stride;
    i32 x, y, z, width, height, depth;
    i32 source_region[6], target_region[6];
    u32 flags;
    u8 scoped_texture_write;
    u8 texture_blit;
} NvnCopyBufferTextureCommand;
#endif
