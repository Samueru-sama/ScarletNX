/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* DRAW_TEXTURE (3D 0x420-0x42B) reaches the renderer once, when its last word is written, with the words in place. */
#include "gpu/gpu.h"
#include "gpu/maxwell/engines_int.h"
#include "gpu/renderer.h"
#include "hle/services/nvdrv.h"
#include "memory/mem.h"
#include "runtime/clock.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <stdio.h>
#include <string.h>

u8* g_mem;
u64 g_draws;
int g_renderer_up = 1;
static int calls;
static u32 seen[12];
static void record(GpuChannel* ch) {
    calls++;
    memcpy(seen, &ch->r3d[0x420], sizeof seen);
}
static const Renderer recording = {.name = "recording", .draw_texture = record};
const Renderer* g_renderer = &recording;
int gmem_read(u64 gva, void* dst, u64 size) {
    (void)gva;
    memset(dst, 0, size);
    return 0;
}
int gmem_write(u64 gva, const void* src, u64 size) { return (void)gva, (void)src, (void)size, 0; }
u32 gmem_r32(u64 gva) { return (void)gva, 0; }
void gmem_w32(u64 gva, u32 v) { (void)gva, (void)v; }
u64 gmmu_translate(u64 gva) { return gva; }
u32 gpu_color_bpp(u32 format) { return (void)format, 4; }
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2) {
    (void)x_bytes, (void)y, (void)z, (void)width_bytes, (void)height, (void)bh_log2, (void)bd_log2;
    return 0;
}
void gpu_signal_point(void) {}
u64 guest_ns(void) { return 0; }
u64 plat_ns(void) { return 0; }
void plat_yield(void) {}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void i2m_launch(GpuChannel* ch, u32* regs, int base) { (void)ch, (void)regs, (void)base; }
void i2m_data(GpuChannel* ch, u32 v) { (void)ch, (void)v; }
void mme_run(GpuChannel* ch, u32 idx, const u32* params, u32 np) { (void)ch, (void)idx, (void)params, (void)np; }
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }

static GpuChannel ch;

int main(void) {
    /* destination (40, 24) 32 x 16 in 20.12, one source texel per pixel in 32.32, sampler 3, texture 5, source (0, 0) */
    const u32 words[12] = {40u << 12, 24u << 12, 32u << 12, 16u << 12, 0, 1, 0, 1, 3, 5, 0, 0};
    int failures = 0;
    for (u32 i = 0; i < 11; i++) maxwell3d_method(&ch, 0x420 + i, words[i], 0);
    if (calls) {
        fputs("FAIL: drawn before its last word\n", stderr);
        failures++;
    }
    maxwell3d_method(&ch, 0x42B, words[11], 1);
    if (calls != 1 || memcmp(seen, words, sizeof words)) {
        fprintf(stderr, "FAIL: %d calls after the last word, or its words weren't in place\n", calls);
        failures++;
    }
    if (failures) return 1;
    puts("DRAW_TEXTURE reaches the renderer once, on its last word, with all its words");
    return 0;
}
