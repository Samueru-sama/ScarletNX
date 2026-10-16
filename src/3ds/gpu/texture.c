/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/texture/texture_decode.cpp (LookupTexture, LookupTexelInTile) and etc1.cpp
   (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar
   a4b335e, with the colour expansions of common/color.h. */
#include "3ds/gpu/texture.h"
#include "3ds/gpu/transfer.h"

unsigned snx_ctr_tile_bytes(unsigned format) {
    switch (format) {
        case SNX_CTR_TEX_RGBA8: return 4 * 64;
        case SNX_CTR_TEX_RGB8: return 3 * 64;
        case SNX_CTR_TEX_RGB5A1:
        case SNX_CTR_TEX_RGB565:
        case SNX_CTR_TEX_RGBA4:
        case SNX_CTR_TEX_IA8:
        case SNX_CTR_TEX_RG8: return 2 * 64;
        case SNX_CTR_TEX_I8:
        case SNX_CTR_TEX_A8:
        case SNX_CTR_TEX_IA4: return 64;
        case SNX_CTR_TEX_I4:
        case SNX_CTR_TEX_A4: return 32;
        case SNX_CTR_TEX_ETC1: return 4 * 8;
        case SNX_CTR_TEX_ETC1A4: return 4 * 16;
        default: return 0;
    }
}

static uint8_t c4to8(unsigned v) { return (uint8_t)((v << 4) | v); }
static uint8_t c5to8(unsigned v) { return (uint8_t)((v << 3) | (v >> 2)); }

/* the Z-order index of (x, y) inside an 8x8 tile (VideoCore::MortonInterleave) */
static unsigned morton(unsigned x, unsigned y) { return snx_ctr_morton_offset(x, y, 1); }

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void snx_ctr_etc1(uint64_t b, unsigned x, unsigned y, uint8_t rgb[3]) {
    static const uint8_t modifiers[8][2] = {{2, 8},   {5, 17},  {9, 29},   {13, 42},
                                            {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    unsigned texel = 4 * x + y;
    int c[3];
    if ((b >> 32) & 1) { /* flip */
        unsigned t = x;
        x = y;
        y = t;
    }
    if ((b >> 33) & 1) { /* differential: 5-bit bases, the second half adds a signed 3-bit delta */
        for (unsigned i = 0; i < 3; ++i) {
            unsigned at = 59 - i * 8; /* r at 59, g at 51, b at 43; deltas 3 bits below */
            c[i] = (int)((b >> at) & 31);
            if (x >= 2) {
                int delta = (int)((b >> (at - 3)) & 7);
                c[i] += delta >= 4 ? delta - 8 : delta;
            }
            c[i] = c5to8((unsigned)c[i] & 0xff);
        }
    } else { /* individual: 4-bit colours per half, r1 at 60, r2 at 56, g1 52, g2 48, b1 44, b2 40 */
        for (unsigned i = 0; i < 3; ++i) c[i] = c4to8((unsigned)(b >> (60 - i * 8 - (x < 2 ? 0 : 4))) & 15);
    }
    unsigned table = x < 2 ? (unsigned)((b >> 37) & 7) : (unsigned)((b >> 34) & 7);
    int modifier = modifiers[table][(b >> texel) & 1];
    if ((b >> (16 + texel)) & 1) modifier = -modifier;
    for (unsigned i = 0; i < 3; ++i) rgb[i] = (uint8_t)clamp255(c[i] + modifier);
}

static uint64_t le64(const unsigned char* p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

bool snx_ctr_texel(SnxCtrMemory* memory, uint32_t address, uint32_t width, unsigned format, unsigned x, unsigned y,
                   uint8_t rgba[4]) {
    unsigned tile = snx_ctr_tile_bytes(format);
    unsigned char p[16];
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    if (!tile) return false;
    /* LookupTexture: tile rows of width / 8 tiles, then the texel inside its tile */
    uint64_t base = (uint64_t)address + (uint64_t)(y / 8) * tile * (width / 8) + (uint64_t)(x / 8) * tile;
    unsigned fx = x % 8, fy = y % 8;
    if (format == SNX_CTR_TEX_ETC1 || format == SNX_CTR_TEX_ETC1A4) {
        bool alpha = format == SNX_CTR_TEX_ETC1A4;
        unsigned size = alpha ? 16 : 8, sub = fx / 4 + 2 * (fy / 4);
        fx %= 4, fy %= 4;
        if (base + sub * size + size > UINT32_MAX ||
            snx_ctr_phys_read(memory, (uint32_t)(base + sub * size), p, size) != SNX_CTR_MEM_OK)
            return false;
        rgba[3] = 255;
        if (alpha) rgba[3] = c4to8((unsigned)(le64(p) >> (4 * (fx * 4 + fy))) & 15);
        snx_ctr_etc1(le64(p + (alpha ? 8 : 0)), fx, fy, rgba);
        return true;
    }
    static const unsigned bytes[12] = {4, 3, 2, 2, 2, 2, 2, 1, 1, 1, 0, 0};
    unsigned index = morton(fx, fy), n = bytes[format] ? bytes[format] : 1;
    uint64_t at = base + (bytes[format] ? index * bytes[format] : index / 2);
    if (at + n > UINT32_MAX || snx_ctr_phys_read(memory, (uint32_t)at, p, n) != SNX_CTR_MEM_OK) return false;
    switch (format) {
        /* the colour formats share the framebuffer codecs (Pica::PixelFormat order: RGB565 before RGB5A1) */
        case SNX_CTR_TEX_RGBA8: snx_ctr_pixel_decode(SNX_CTR_PIX_RGBA8, p, rgba); break;
        case SNX_CTR_TEX_RGB8: snx_ctr_pixel_decode(SNX_CTR_PIX_RGB8, p, rgba); break;
        case SNX_CTR_TEX_RGB5A1: snx_ctr_pixel_decode(SNX_CTR_PIX_RGB5A1, p, rgba); break;
        case SNX_CTR_TEX_RGB565: snx_ctr_pixel_decode(SNX_CTR_PIX_RGB565, p, rgba); break;
        case SNX_CTR_TEX_RGBA4: snx_ctr_pixel_decode(SNX_CTR_PIX_RGBA4, p, rgba); break;
        case SNX_CTR_TEX_IA8: rgba[0] = rgba[1] = rgba[2] = p[1], rgba[3] = p[0]; break;
        case SNX_CTR_TEX_RG8: rgba[0] = p[1], rgba[1] = p[0], rgba[2] = 0, rgba[3] = 255; break; /* DecodeRG8 */
        case SNX_CTR_TEX_I8: rgba[0] = rgba[1] = rgba[2] = p[0], rgba[3] = 255; break;
        case SNX_CTR_TEX_A8: rgba[3] = p[0]; break;
        case SNX_CTR_TEX_IA4:
            rgba[0] = rgba[1] = rgba[2] = c4to8(p[0] >> 4);
            rgba[3] = c4to8(p[0] & 15);
            break;
        case SNX_CTR_TEX_I4:
            rgba[0] = rgba[1] = rgba[2] = c4to8(index % 2 ? p[0] >> 4 : p[0] & 15);
            rgba[3] = 255;
            break;
        case SNX_CTR_TEX_A4: rgba[3] = c4to8(index % 2 ? p[0] >> 4 : p[0] & 15); break;
        default: return false;
    }
    return true;
}
