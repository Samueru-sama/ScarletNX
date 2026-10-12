/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_COPY_H
#define SNX_NVN_COPY_H
#include "gpu/nvn_texture.h"

typedef struct {
    u64 sequence;
    NvnTextureTarget target;
    u64 source;
    i64 row_stride, image_stride;
    i32 x, y, z, width, height, depth;
    u32 flags;
} NvnCopyBufferTextureCommand;
#endif
