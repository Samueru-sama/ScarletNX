/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the 3D engine's side of the renderer: clears, draws, constant buffers and blits go to it, with clears done in guest
   memory before the renderer starts. Also the record of where the GPU wrote to memory */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/maxwell/write_pages.h"
#include "nx/gpu/renderer.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/gpu/gpu.h"
void r3d_clear(GpuChannel* ch, u32 v) {
    if (ch->render_off) return; /* SetRenderEnable says no */
    if (g_renderer_up) {
        g_renderer->clear(ch, v);
        return;
    }
    u32* r = ch->r3d;
    u32 rt = (v >> 6) & 15;
    if (!(v & 0x3C)) return;
    u32* t = &r[0x200 + rt * 0x10];
    u64 addr = ((u64)t[0] << 32) | t[1];
    u32 w = t[2], h = t[3], fmt = t[4], tm = t[5];
    if (!addr || !w || !h) return;
    float cc[4];
    memcpy(cc, &r[0x360], 16);
    u32 bpp = gpu_color_bpp(fmt);
    u8 px[16];
    if (bpp == 4) {
        u32 c = 0;
        for (int i = 0; i < 4; i++) {
            float f = cc[i] < 0 ? 0 : cc[i] > 1 ? 1 : cc[i];
            c |= (u32)(f * 255.f + 0.5f) << (i * 8);
        }
        if (fmt == 0xCF || fmt == 0xD0) c = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
        memcpy(px, &c, 4);
    } else
        memset(px, 0, 16);
    int linear = (tm >> 12) & 1;
    u32 bh = (tm >> 4) & 15;
    /* a pitch-linear target holds its row pitch in bytes where the width is (as gpu/vk/render.c reads it) */
    u32 pitch = linear ? w : w * bpp, wb = linear ? w / bpp * bpp : w * bpp;
    if (!wb) return;
    u8* row = malloc(wb);
    if (!row) {
        WARN("gpu: no memory to clear a %ux%u target\n", w, h);
        return;
    }
    for (u32 x = 0; x < wb; x += bpp) memcpy(row + x, px, bpp);
    if (linear) {
        for (u32 y = 0; y < h; y++) gmem_write(addr + (u64)y * pitch, row, wb);
    } else { /* 16 bytes at a time, logged once as the whole range */
        u64 hi = 0;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < wb; x += 16) {
                u64 o = bl_offset(x, y, 0, wb, h, bh, 0), cpu = gmmu_translate(addr + o);
                u32 n = wb - x < 16 ? wb - x : 16;
                if (cpu) memcpy(GP(cpu), row + x, n);
                if (o + n > hi) hi = o + n;
            }
        gpu_mem_written(addr, hi);
    }
    free(row);
}
void r3d_draw_texture(GpuChannel* ch) {
    if (g_renderer_up && g_renderer->draw_texture) g_renderer->draw_texture(ch);
}
void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances) {
    if (ch->render_off) return; /* SetRenderEnable says no */
    /* SET_PRIMITIVE_TOPOLOGY_CONTROL selects the separate topology register for every draw path. */
    if ((ch->r3d[0x652] & 1) && ch->r3d[0x65C]) {
        /* The override register has a different enum for points and lines than BEGIN. */
        u32 override = ch->r3d[0x65C] & 0xF;
        topology = override == 1 ? 0 : override == 2 ? 1 : override;
    }
    if (g_renderer_up) {
        g_renderer->draw(ch, indexed, topology, first, count, base_vertex, base_instance, instances);
        return;
    }
    (void)ch;
    (void)indexed;
    (void)topology;
    (void)first;
    (void)count;
    (void)base_vertex;
    (void)base_instance;
    (void)instances;
    g_draws++;
}
/* constant buffer data comes a word per method: the words of a method batch are collected and written at once, when
   the batch ends or before any other method (maxwell3d_method, call_method) and at the end of a submission */
void r3d_const_upload(GpuChannel* ch, u32 v, int last) {
    u32* r = ch->r3d;
    if (ch->cb_n == sizeof ch->cb_words / sizeof ch->cb_words[0]) r3d_const_flush(ch);
    if (!ch->cb_n) ch->cb_addr = (((u64)r[0x8E1] << 32) | r[0x8E2]) + r[0x8E3];
    ch->cb_words[ch->cb_n++] = v;
    r[0x8E3] += 4;
    if (last) r3d_const_flush(ch);
}
void r3d_const_flush(GpuChannel* ch) {
    if (!ch->cb_n) return;
    gmem_write(ch->cb_addr, ch->cb_words, (u64)ch->cb_n * 4);
    ch->cb_n = 0;
}
/* a constant buffer bound to a stage: v bit 0 valid, bits 4-8 the slot; address and size from 0x8E0-0x8E2. The
   binding is the channel's 3D state, which the renderer reads at each draw: another channel's binds, run while this
   one's stream waits (gpu_park), don't change it */
void r3d_cb_bind(GpuChannel* ch, int stage, u32 v) {
    u32* r = ch->r3d;
    u32 idx = (v >> 4) & 31;
    if (stage > 4 || idx >= 18) return;
    ch->cbind[stage][idx] = (v & 1) ? ((u64)r[0x8E1] << 32) | r[0x8E2] : 0;
    ch->cbind_size[stage][idx] = (v & 1) ? r[0x8E0] : 0;
}
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy) {
    if (g_renderer_up) return g_renderer->blit(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    return 0;
}

/* last write gen of each 4K page for GPU-side writes (cb_data, I2M, DMA, 2D, reports and shader writeback), so we can
   tell a range changed without rehashing it. Exact per page: the table this replaced hashed pages 256 MiB apart into
   one slot and handed ranges over 1 MiB to a 1024-entry log that constant buffer updates wrap every frame, so most
   large textures read as written at every use (TotK hashed ~200 GB of textures again in two minutes for 13 that had
   changed) */
static u64 g_gw_gen;      /* published after each complete update */
static u64 g_gw_phys_gen; /* latest GPU write to a guest CPU page */
static PMutex g_gwlock = PMUTEX_INIT;
static WritePages g_gw_va, g_gw_phys; /* by GPU address and by guest backing page */
static void mem_written(u64 gva, u64 size) {
    if (!size) return;
    pm_lock(&g_gwlock);
    u64 gen = g_gw_gen + 1;
    write_pages_stamp(&g_gw_va, gva, size, gen);
    for (u64 b = gva >> 12, e = (gva + size - 1) >> 12; b <= e; b++) {
        u64 cpu = gmmu_translate(b << 12);
        if (cpu) write_pages_stamp(&g_gw_phys, cpu & ~0xFFFull, 1, gen);
    }
    g_gw_phys_gen = gen;
    __atomic_store_n(&g_gw_gen, gen, __ATOMIC_RELEASE);
    pm_unlock(&g_gwlock);
}
void gpu_mem_written(u64 gva, u64 size) { mem_written(gva, size); }
void gpu_mem_written_shader(u64 gva, u64 size) { mem_written(gva, size); }
u64 gpu_mem_generation(void) { return __atomic_load_n(&g_gw_gen, __ATOMIC_ACQUIRE); }
static int phys_dirty_since(u64 since, u64 a, u64 sz) {
    if (!sz || since >= g_gw_phys_gen) return 0;
    for (u64 b = a >> 12, e = (a + sz - 1) >> 12; b <= e; b++) {
        u64 cpu = gmmu_translate(b << 12);
        if (cpu && write_pages_since(&g_gw_phys, since, cpu, 1)) return 1;
    }
    return 0;
}
/* nonzero if [a,a+sz) may have been written after gen 'since' */
static int dirty_since_locked(u64 since, u64 a, u64 sz) {
    if (!sz || since >= g_gw_gen) return 0;
    return write_pages_since(&g_gw_va, since, a, sz) || phys_dirty_since(since, a, sz);
}
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    pm_lock(&g_gwlock);
    int dirty = dirty_since_locked(since, a, sz);
    pm_unlock(&g_gwlock);
    return dirty;
}
