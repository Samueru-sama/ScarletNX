/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   PICA200 texture formats: one texel looked up in a tiled texture in physical memory, as Azahar
   video_core/texture/texture_decode.cpp and etc1.cpp (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator
   Project / Azahar Emulator Project, at azahar-emu/azahar a4b335e) decode it. */
#ifndef SNX_CTR_TEXTURE_H
#define SNX_CTR_TEXTURE_H
#include "3ds/memory/ctr_memory.h"
#include <stdbool.h>
#include <stdint.h>

/* TexturingRegs::TextureFormat */
enum {
    SNX_CTR_TEX_RGBA8,
    SNX_CTR_TEX_RGB8,
    SNX_CTR_TEX_RGB5A1,
    SNX_CTR_TEX_RGB565,
    SNX_CTR_TEX_RGBA4,
    SNX_CTR_TEX_IA8,
    SNX_CTR_TEX_RG8,
    SNX_CTR_TEX_I8,
    SNX_CTR_TEX_A8,
    SNX_CTR_TEX_IA4,
    SNX_CTR_TEX_I4,
    SNX_CTR_TEX_A4,
    SNX_CTR_TEX_ETC1,
    SNX_CTR_TEX_ETC1A4
};
/* bytes of one 8x8 tile (CalculateTileSize); 0 for the formats past ETC1A4 */
unsigned snx_ctr_tile_bytes(unsigned format);
/* LookupTexture: the texel at (x, y) of a width-wide texture at address, x and y in memory order (the caller flips
   t). False, with transparent black, when the texel is outside the board's memory or the format is unknown */
bool snx_ctr_texel(SnxCtrMemory* memory, uint32_t address, uint32_t width, unsigned format, unsigned x, unsigned y,
                   uint8_t rgba[4]);
/* one ETC1 4x4 subtile's colour at (x, y) (SampleETC1Subtile) */
void snx_ctr_etc1(uint64_t block, unsigned x, unsigned y, uint8_t rgb[3]);
#endif
