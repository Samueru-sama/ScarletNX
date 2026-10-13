/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* VIC: the video image compositor. Here it copies each enabled slot's NV12 surface (the picture NVDEC wrote there)
   into the game's surface, converted to its format, without scaling: a picture larger than its target rectangle or
   the surface is cropped.
   How the pixels are composed follows Eden's VIC (eden-emulator/mirror, src/video_core/host1x/vic.cpp): a slot is
   read into 10 bit pixels (its Y, U, V samples scaled to 10 bit, its alpha from the slot's planar alpha), the slot's
   color matrix and soft clamp are applied when the config enables them, and the result is written back at 8 bit per
   channel. Without a color matrix the samples are copied as they are (Y, U, V land in the R, G, B channels): VIC
   does no fixed YUV to RGB conversion of its own, the color conversion is whatever matrix the config carries. */
#include "gpu/video/host1x_int.h"
/* The config struct (its iova in register 0x708) as the game's TVMR and NvDdkVic libraries build it: 0x610 bytes,
   the output surface config at +0x20, 8 slots of 0xB0 bytes from +0x90.
   Surface config u64 (output at +0x20, a slot's at slot+0x40): 0-6 format (0x1F A8B8G8R8, 0x20 A8R8G8B8,
   0x23 X8B8G8R8, 0x44 NV12), 11-14 block kind (0 pitch linear), 15-18 block height log2, 32-45 width-1,
   46-59 height-1. Its other u64s name the luma and chroma planes' padded sizes (width-1 in bits 0-13,
   height-1 in bits 14-27).
   The output config (at +0x10) carries the target rectangle, inclusive pixel coordinates: left 0-13 and right
   16-29 of the u32 at +0x18, top 0-13 and bottom 16-29 of the one at +0x1C. The slots' destination rectangles
   are clipped to it.
   A slot's config u64 at slot+0: bit 0 enable. Its source rectangle is 16.16 fixed point, right and bottom inclusive:
   left bits 0-29 and right 32-61 of the u64 at slot+0x20, top and bottom of the one at +0x28. Its target rectangle,
   u64 at slot+0x30: left 0-13, right 16-29, top 32-45, bottom 48-61, also inclusive.
   The u64 at slot+0x10 has the soft clamp range the converted pixels are clamped to (low 0-9, high 10-19) and the
   planar alpha (32-41), the 10 bit alpha every pixel of the slot gets.
   A slot's color matrix, four u64s at slot+0x60, has three signed 20 bit coefficients per u64 (S12.8), the right
   shift in bits 60-63 of the first u64 and the enable bit at bit 63 of the second (Eden's MatrixStruct).
   A slot's surface 0 (the current frame) is at registers 0x400 + slot * 0x60 (luma) and +4 (chroma), iova >> 8 */
#define VIC_CFG_SIZE 0x610
#define VIC_NUM_SLOTS 8
#define VIC_MAX_ROW 0x4000 /* one row of 14 bit widths: the largest a surface config names */

/* a 10 bit pixel as Eden's Pixel: R, G, B hold the slot's Y, U, V samples scaled to 10 bit, A the planar alpha */
typedef struct {
    u16 r, g, b, a;
} VicPixel;

/* a slot's color matrix (Eden's MatrixStruct): out = (R*c0 + G*c1 + B*c2) >> r_shift, then + c3, then >> 8 */
typedef struct {
    int c[3][4]; /* [row][column], the fourth column is added after the shift */
    u32 r_shift;
    int enable;
} VicMatrix;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* a signed 20 bit coefficient, as the config stores the matrix */
static int sext20(u64 v) {
    u32 x = (u32)v & 0xFFFFF;
    return (x & 0x80000) ? (int)x - 0x100000 : (int)x;
}
static void matrix_read(VicMatrix* m, const u8* p) {
    u64 w0 = rd64(p, 0), w1 = rd64(p, 8), w2 = rd64(p, 16), w3 = rd64(p, 24);
    m->enable = (int)(w1 >> 63);
    m->r_shift = (u32)((w0 >> 60) & 15);
    m->c[0][0] = sext20(w0);
    m->c[1][0] = sext20(w0 >> 20);
    m->c[2][0] = sext20(w0 >> 40);
    m->c[0][1] = sext20(w1);
    m->c[1][1] = sext20(w1 >> 20);
    m->c[2][1] = sext20(w1 >> 40);
    m->c[0][2] = sext20(w2);
    m->c[1][2] = sext20(w2 >> 20);
    m->c[2][2] = sext20(w2 >> 40);
    m->c[0][3] = sext20(w3);
    m->c[1][3] = sext20(w3 >> 20);
    m->c[2][3] = sext20(w3 >> 40);
}
/* one slot row of pixels through the matrix and the soft clamp, as Eden's Blend does; without an enabled
   matrix the pixels are left alone (the samples are copied as they are). The alpha is left as the planar
   alpha, as Eden's main (SSE) path keeps it out of the matrix and the clamp */
static void pixels_convert(VicPixel* px, u32 n, const VicMatrix* m, int lo, int hi) {
    u32 i;
    if (!m->enable) return;
    for (i = 0; i < n; i++) {
        int r = (int)px[i].r * m->c[0][0] + (int)px[i].g * m->c[0][1] + (int)px[i].b * m->c[0][2];
        int g = (int)px[i].r * m->c[1][0] + (int)px[i].g * m->c[1][1] + (int)px[i].b * m->c[1][2];
        int b = (int)px[i].r * m->c[2][0] + (int)px[i].g * m->c[2][1] + (int)px[i].b * m->c[2][2];
        r = ((r >> m->r_shift) + m->c[0][3]) >> 8;
        g = ((g >> m->r_shift) + m->c[1][3]) >> 8;
        b = ((b >> m->r_shift) + m->c[2][3]) >> 8;
        px[i].r = (u16)clampi(r, lo, hi);
        px[i].g = (u16)clampi(g, lo, hi);
        px[i].b = (u16)clampi(b, lo, hi);
    }
}

/* A surface plane as VIC addresses it: pitch linear rows (256 byte aligned) or block linear GOBs */
typedef struct {
    u8* p;
    u64 lim; /* bytes p may touch */
    u32 blk, bh, pitch, h;
} Plane;
static int plane_map(Plane* pl, u64 iova, u32 blk, u32 bh, u32 wbytes, u32 h) {
    pl->blk = blk;
    pl->bh = bh;
    pl->h = h;
    if (blk) {
        /* whole GOBs: 64 bytes wide (bl_offset rounds the width up the same way), 8 << bh rows high */
        u32 gy = 8u << bh;
        pl->pitch = wbytes;
        pl->lim = (u64)((wbytes + 63) & ~63u) * ((h + gy - 1) / gy * gy);
    } else {
        pl->pitch = (wbytes + 255) & ~255u;
        pl->lim = (u64)pl->pitch * h;
    }
    pl->p = iova ? iova_ptr(iova, pl->lim) : NULL;
    if (!pl->p && iova && blk) {
        /* a buffer that ends inside the last block row: the rows past its end are left out */
        pl->lim = (u64)wbytes * h;
        pl->p = iova_ptr(iova, pl->lim);
    }
    return pl->p != NULL;
}
/* n bytes of row y from byte xb, between the plane and buf: block linear moves at most up to the next 16 byte
   boundary at a time, the run bl_offset keeps contiguous */
static void plane_row(const Plane* pl, u32 xb, u32 y, u8* buf, u32 n, int write) {
    for (u32 k = 0; k < n;) {
        u32 x = xb + k, run = pl->blk ? 16 - (x & 15) : n - k;
        if (run > n - k) run = n - k;
        u64 off = pl->blk ? bl_offset(x, y, 0, pl->pitch, pl->h, pl->bh, 0) : (u64)y * pl->pitch + x;
        if (off + run <= pl->lim) {
            if (write)
                memcpy(pl->p + off, buf + k, run);
            else
                memcpy(buf + k, pl->p + off, run);
        } else if (!write)
            memset(buf + k, 0, run);
        k += run;
    }
}
/* one row of 14 bit widths: the largest a surface config names */
static u8 g_rowy[VIC_MAX_ROW], g_rowc[VIC_MAX_ROW], g_rowo[VIC_MAX_ROW * 4];
/* the slot row as 10 bit pixels */
static VicPixel g_slotrow[VIC_MAX_ROW];

/* slot s of config cs into the output surface. 1 when it wrote anything */
static int compose_slot(const u32* r, const u8* cs, int s, u32 fmt, const Plane* oy, const Plane* oc, u32 sw, u32 sh) {
    const u8* sl = cs + 0x90 + s * 0xB0;
    u64 sc = rd64(sl, 0x40);
    u32 ifmt = (u32)(sc & 0x7F), iblk = (u32)((sc >> 11) & 15), ibh = (u32)((sc >> 15) & 15);
    u32 iw = (u32)((sc >> 32) & 0x3FFF) + 1, ih = (u32)((sc >> 46) & 0x3FFF) + 1;
    if (ifmt != 0x44) {
        LOG_LIMIT(4, "video: VIC slot %d input format %x isn't handled (NV12 is), slot skipped\n", s, ifmt);
        return 0;
    }
    u64 src_x = rd64(sl, 0x20), src_y = rd64(sl, 0x28), dst = rd64(sl, 0x30);
    u32 sx0 = (u32)(src_x & 0x3FFFFFFF) >> 16, sx1 = (u32)((src_x >> 32) & 0x3FFFFFFF) >> 16;
    u32 sy0 = (u32)(src_y & 0x3FFFFFFF) >> 16, sy1 = (u32)((src_y >> 32) & 0x3FFFFFFF) >> 16;
    u32 dx0 = (u32)(dst & 0x3FFF), dx1 = (u32)((dst >> 16) & 0x3FFF);
    u32 dy0 = (u32)((dst >> 32) & 0x3FFF), dy1 = (u32)((dst >> 48) & 0x3FFF);
    if (sx1 < sx0 || sy1 < sy0 || dx1 < dx0 || dy1 < dy0 || sx0 >= iw || sy0 >= ih || dx0 >= sw || dy0 >= sh) {
        LOG_LIMIT(4, "video: VIC slot %d rectangles are empty or outside the surfaces, slot skipped\n", s);
        return 0;
    }
    u32 w = sx1 - sx0 + 1, h = sy1 - sy0 + 1; /* the source rectangle */
    u32 dw = dx1 - dx0 + 1, dh = dy1 - dy0 + 1; /* the destination rectangle */
    if (w != dw || h != dh)
        LOG_LIMIT(2, "video: VIC slot %d scales %ux%u to %ux%u, which isn't done: the picture is cropped\n", s, w, h,
                  dw, dh);
    if (w > dw) w = dw;
    if (h > dh) h = dh;
    /* the target rectangle of the output config clips the destination, like Eden's Blend, and the picture is
       cropped from the side the crop lands on */
    u32 tgt0 = rd32(cs, 0x18), tgt1 = rd32(cs, 0x1C);
    u32 tx0 = tgt0 & 0x3FFF, tx1 = (tgt0 >> 16) & 0x3FFF, ty0 = tgt1 & 0x3FFF, ty1 = (tgt1 >> 16) & 0x3FFF;
    if (tx1 < tx0 || ty1 < ty0) {
        LOG_LIMIT(4, "video: VIC slot %d target rectangle is empty, slot skipped\n", s);
        return 0;
    }
    if (dx0 > tx1 || dy0 > ty1) {
        LOG_LIMIT(4, "video: VIC slot %d rectangles are outside the target rectangle, slot skipped\n", s);
        return 0;
    }
    if (dx0 < tx0) {
        u32 cut = tx0 - dx0;
        if (cut >= w) {
            LOG_LIMIT(4, "video: VIC slot %d rectangles are outside the target rectangle, slot skipped\n", s);
            return 0;
        }
        sx0 += cut;
        dx0 += cut;
        w -= cut;
    }
    if (dy0 < ty0) {
        u32 cut = ty0 - dy0;
        if (cut >= h) {
            LOG_LIMIT(4, "video: VIC slot %d rectangles are outside the target rectangle, slot skipped\n", s);
            return 0;
        }
        sy0 += cut;
        dy0 += cut;
        h -= cut;
    }
    if (w > tx1 - dx0 + 1) w = tx1 - dx0 + 1;
    if (h > ty1 - dy0 + 1) h = ty1 - dy0 + 1;
    if (w > sw - dx0) w = sw - dx0;
    if (h > sh - dy0) h = sh - dy0;
    if (w > iw - sx0) w = iw - sx0;
    if (h > ih - sy0) h = ih - sy0;
    if (!w || !h) {
        LOG_LIMIT(4, "video: VIC slot %d rectangles are empty or outside the surfaces, slot skipped\n", s);
        return 0;
    }
    /* 4:2:0 chroma covers pixel pairs: the rectangles start on even pixels */
    sx0 &= ~1u;
    sy0 &= ~1u;
    dx0 &= ~1u;
    dy0 &= ~1u;
    /* the slot's soft clamp range and planar alpha (the u64 at slot+0x10), and its color matrix */
    u64 cfg2 = rd64(sl, 0x10);
    int clamp_lo = (int)(cfg2 & 0x3FF), clamp_hi = (int)((cfg2 >> 10) & 0x3FF);
    u16 alpha = (u16)((cfg2 >> 32) & 0x3FF);
    VicMatrix mat;
    matrix_read(&mat, sl + 0x60);
    Plane iy, ic;
    u64 ly = (u64)r[(0x400 + s * 0x60) / 4] << 8, lc = (u64)r[(0x404 + s * 0x60) / 4] << 8;
    if (!plane_map(&iy, ly, iblk, ibh, iw, ih) || !plane_map(&ic, lc, iblk, ibh, iw, (ih + 1) / 2)) {
        LOG_LIMIT(4, "video: VIC slot %d input (luma iova %llx, chroma %llx) not mapped, slot skipped\n", s,
                  (unsigned long long)ly, (unsigned long long)lc);
        return 0;
    }
    LOG_LIMIT(3, "video: VIC slot %d: NV12 %ux%u %s (block height %u), source %u,%u, target %u,%u, %ux%u copied\n", s,
              iw, ih, iblk ? "block linear" : "pitch linear", 1u << ibh, sx0, sy0, dx0, dy0, w, h);
    u32 wc = (w + 1) & ~1u; /* chroma bytes: whole U, V pairs */
    int swap = fmt == 0x20; /* A8R8G8B8 stores R and B the other way around than A8B8G8R8 does */
    for (u32 y = 0; y < h; y++) {
        /* the slot's row as 10 bit pixels, as Eden's ReadProgressiveY8__V8U8_N420 builds them: the samples
           scaled to 10 bit (chroma samples duplicated over the pixel pairs), the planar alpha as the alpha */
        plane_row(&iy, sx0, sy0 + y, g_rowy, w, 0);
        u32 csx = sx0 & ~1u, cend = (sx0 + w + 1) & ~1u;
        plane_row(&ic, csx, (sy0 + y) / 2, g_rowc, cend - csx, 0);
        for (u32 k = 0; k < w; k++) {
            u32 cb = ((sx0 + k) & ~1u) - csx;
            g_slotrow[k].r = (u16)(g_rowy[k] << 2);
            g_slotrow[k].g = (u16)(g_rowc[cb] << 2);
            g_slotrow[k].b = (u16)(g_rowc[cb + 1] << 2);
            g_slotrow[k].a = alpha;
        }
        pixels_convert(g_slotrow, w, &mat, clamp_lo, clamp_hi);
        u32 out_y = dy0 + y;
        if (fmt != 0x44) {
            /* the 10 bit channels back to 8 bit, as Eden's WriteABGR writes them */
            for (u32 k = 0; k < w; k++) {
                u8* o = g_rowo + k * 4;
                u8 rr = (u8)(g_slotrow[k].r >> 2), gg = (u8)(g_slotrow[k].g >> 2);
                u8 bb = (u8)(g_slotrow[k].b >> 2), aa = (u8)(g_slotrow[k].a >> 2);
                o[0] = swap ? bb : rr;
                o[1] = gg;
                o[2] = swap ? rr : bb;
                o[3] = aa;
            }
            plane_row(oy, dx0 * 4, out_y, g_rowo, w * 4, 1);
        } else {
            /* NV12 out: luma from R, chroma from G and B of the pixel pairs */
            for (u32 k = 0; k < w; k++) g_rowo[k] = (u8)(g_slotrow[k].r >> 2);
            plane_row(oy, dx0, out_y, g_rowo, w, 1);
            /* the chroma rows follow the luma pairs; the row keeps the chroma of its even row */
            if ((out_y & 1) == 0 || y == 0) {
                for (u32 k = 0; k < wc; k += 2) {
                    g_rowo[k] = (u8)(g_slotrow[k].g >> 2);
                    g_rowo[k + 1] = (u8)(g_slotrow[k].b >> 2);
                }
                plane_row(oc, dx0, out_y / 2, g_rowo, wc, 1);
            }
        }
    }
    return 1;
}
void vic_execute(const u32* r) {
    {
        static u32 nv;
        nv++;
        if (nv % 120 == 0) LOG("video: %u VIC jobs so far\n", nv);
    }
    u64 ly = (u64)r[0x720 / 4] << 8, lc = (u64)r[0x724 / 4] << 8, cfg_iova = (u64)r[0x708 / 4] << 8;
    u8* cs = iova_ptr(cfg_iova, VIC_CFG_SIZE);
    if (!cs) {
        /* without the config the surface's format, size and layout are unknown: writing a guess corrupts it */
        LOG_LIMIT(4, "video: VIC config at iova %llx not mapped, job skipped\n", (unsigned long long)cfg_iova);
        return;
    }
    u64 cfg = rd64(cs, 0x20);
    u32 fmt = (u32)(cfg & 0x7F), blk = (u32)((cfg >> 11) & 15), bh = (u32)((cfg >> 15) & 15);
    u32 sw = (u32)((cfg >> 32) & 0x3FFF) + 1, sh = (u32)((cfg >> 46) & 0x3FFF) + 1;
    /* the surface config's luma and chroma sizes bound what is written, as Eden's WriteABGR and WriteY8__V8U8_N420
       clamp their output to them */
    u64 planes = rd64(cs, 0x28);
    u32 lw = (u32)(planes & 0x3FFF) + 1, lh = (u32)((planes >> 14) & 0x3FFF) + 1;
    if (lw < sw) sw = lw;
    if (lh < sh) sh = lh;
    /* SNX_VIC_FMT=n overrides the output format the config names, to try another one while debugging */
    static int fmt_env = -1;
    if (fmt_env == -1) {
        const char* e = getenv("SNX_VIC_FMT");
        fmt_env = e ? (int)(strtoul(e, 0, 0) & 0x7F) : -2;
    }
    if (fmt_env >= 0) fmt = (u32)fmt_env;
    if (fmt != 0x1F && fmt != 0x20 && fmt != 0x23 && fmt != 0x44) {
        LOG_LIMIT(4, "video: VIC output format %x isn't handled, job skipped\n", fmt);
        return;
    }
    LOG_LIMIT(3, "video: VIC config %016llx: format %x, %s (block height %u), surface %ux%u\n", (unsigned long long)cfg,
              fmt, blk ? "block linear" : "pitch linear", 1u << bh, sw, sh);
    Plane oy, oc = {0};
    if (!plane_map(&oy, ly, blk, bh, fmt == 0x44 ? sw : sw * 4, sh)) {
        LOG_LIMIT(4, "video: VIC output at iova %llx not mapped\n", (unsigned long long)ly);
        return;
    }
    if (fmt == 0x44 && !plane_map(&oc, lc, blk, bh, sw, (sh + 1) / 2))
        LOG_LIMIT(4, "video: VIC output chroma at iova %llx not mapped, the picture has no color\n",
                  (unsigned long long)lc);
    /* the slots in order, a later one over an earlier one (no blending) */
    int any = 0;
    for (int s = 0; s < VIC_NUM_SLOTS; s++)
        if (rd64(cs, 0x90 + s * 0xB0) & 1) any |= compose_slot(r, cs, s, fmt, &oy, &oc, sw, sh);
    if (!any) LOG_LIMIT(4, "video: VIC job with no slot it could compose\n");
}
