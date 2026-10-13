/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* VERTEX_ARRAY_INSTANCE_FIRST and _SUBSEQUENT (3D 0x485, 0x486): the 3D engine's methods run against a renderer that
   records the draws reaching it. FIRST draws its range as instance 0; each SUBSEQUENT of the same range is one more
   instance of that draw. */
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
typedef struct {
    int indexed;
    u32 topology, first, count, instances;
} Draw;
static Draw draws[8];
static int ndraws;
static void record_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex,
                        u32 base_instance, u32 instances) {
    (void)ch, (void)base_vertex, (void)base_instance;
    if (ndraws < 8) draws[ndraws] = (Draw){indexed, topology, first, count, instances};
    ndraws++;
}
static void no_clear(GpuChannel* ch, u32 v) { (void)ch, (void)v; }
static const Renderer recording = {.name = "recording", .clear = no_clear, .draw = record_draw};
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
static int failures;
static u32 word(u32 topology, u32 first, u32 count) { return first | count << 16 | topology << 28; }
static void expect(int n, const char* what) {
    r3d_pending_flush(); /* what something else in the stream would do */
    if (ndraws != n) {
        fprintf(stderr, "FAIL: %s: %d draws, expected %d\n", what, ndraws, n);
        failures++;
    }
}
static void expect_draw(int i, u32 topology, u32 first, u32 count, u32 instances, const char* what) {
    const Draw* d = &draws[i];
    if (i >= ndraws || d->indexed || d->topology != topology || d->first != first || d->count != count ||
        d->instances != instances) {
        fprintf(stderr, "FAIL: %s: draw %d is topology %u first %u count %u x%u, expected %u %u %u x%u\n", what, i,
                d->topology, d->first, d->count, d->instances, topology, first, count, instances);
        failures++;
    }
}

int main(void) {
    maxwell3d_method(&ch, 0x485, word(4, 0, 6), 1);
    expect(1, "FIRST draws");
    expect_draw(0, 4, 0, 6, 1, "FIRST is one instance of its range");

    ndraws = 0;
    maxwell3d_method(&ch, 0x485, word(4, 6, 6), 1);
    for (int i = 0; i < 3; i++) maxwell3d_method(&ch, 0x486, word(4, 6, 6), 1);
    expect(1, "FIRST and three SUBSEQUENT are one draw");
    expect_draw(0, 4, 6, 6, 4, "FIRST and three SUBSEQUENT are four instances");

    ndraws = 0;
    maxwell3d_method(&ch, 0x485, word(4, 0, 6), 1);
    maxwell3d_method(&ch, 0x486, word(5, 0, 6), 1);
    expect(2, "a SUBSEQUENT of another topology is a draw of its own");
    expect_draw(0, 4, 0, 6, 1, "the FIRST before it");
    expect_draw(1, 5, 0, 6, 2, "instances 0 and 1 of the SUBSEQUENT");

    if (failures) return 1;
    puts("VERTEX_ARRAY_INSTANCE_FIRST draws its range; each SUBSEQUENT of it is one more instance");
    return 0;
}
