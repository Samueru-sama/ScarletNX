/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_CLEAR_H
#define SNX_NVN_CLEAR_H
#include "gpu/nvn_texture.h"

typedef enum { NVN_CLEAR_COLOR, NVN_CLEAR_DEPTH_STENCIL } NvnClearKind;

/* A self-contained NVN clear. The selected target is a texture, not Maxwell 3D register state. */
typedef struct {
    u64 sequence;
    u64 unknown_marker, prior_unmodeled_write;
    NvnClearKind kind;
    NvnTextureTarget target;
    u32 color[4], mask;
    u32 depth_bits, stencil, depth_mask, stencil_mask;
    i32 region[6]; /* ClearTexture's x/y/z and width/height/depth; other clears use the full target */
    u8 region_known;
} NvnClearCommand;
#endif
