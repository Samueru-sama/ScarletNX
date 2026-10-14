/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/texture/depth.h"

/* Where a texel keeps its depth, by the formats' names in deko3d's ImageFormat and Eden's format table (the last named
   component is in the low bits): G24R8 and Z24S8 have stencil in the low byte and depth above it, X8Z24 and S8Z24
   depth in the low 24 bits and the stencil (or nothing) above */
u32 depth24_unpack(u32 tic_format, u32 texel, u8* stencil) {
    if (tic_format == 0x2A || tic_format == 0x2B) {
        *stencil = tic_format == 0x2B ? (u8)(texel >> 24) : 0;
        return texel & 0xFFFFFF;
    }
    *stencil = (u8)texel;
    return texel >> 8;
}
