/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory ranges as buffers the GPU reads: vertices, indices and storage buffers. A range is checked once per
   guest submit (and again after the GPU front end wrote it). Data that changed is copied into the frame's staging
   ring, which draws read directly; data that stayed the same over a few checks gets a buffer of its own, written
   once and never changed (a change makes a new one), so nothing waits for the GPU. Big ranges that have been stable
   for long are checked less often. */
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "hle/services/nvdrv.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include "gpu/vk/vk.h"

typedef struct Vb {
    u64 start, len, hash, shash;
    u32 epoch, strict_epoch, chk_epoch, schk, stable, used;
    u64 gw;    /* GPU-side write log position at the last check */
    u64 frame; /* the staging frame buf/off point into, when not own */
    int own;   /* buf is this entry's own buffer */
    u32 wseen; /* write-tracking clock when it last matched memory */
    VkBuffer buf;
    VmaAllocation mem;
    u64 off;
    struct Vb* next;
} Vb;
#define VBH 4096
static Vb* g_vbh[VBH];
/* Optional copy telemetry. Counters are updated under vk_lock. */
static u64 g_copy_stage, g_copy_owned, g_copy_uncached, g_copy_zero, g_copy_calls, g_copy_peak, g_read_bound;
static u64 g_wb_outputs, g_wb_bytes, g_wb_waits, g_wb_wait_ns, g_wb_reuses;
static u64 g_alias_draws, g_alias_pages;
enum { WB_DRAW, WB_READ, WB_WRITE, WB_SIGNAL, WB_SYNCWAIT, WB_MAP, WB_REASON_N };
static u64 g_wb_reason[WB_REASON_N], g_wb_reason_ns[WB_REASON_N], g_wb_draw_miss[4];
static int g_copy_stats = -1;
static int copy_stats_on(void) {
    if (g_copy_stats < 0) g_copy_stats = getenv("SNX_VK_BUFFER_STATS") != NULL;
    return g_copy_stats;
}
static void copy_note(u64 gva, u64 len) {
    if (!copy_stats_on()) return;
    g_copy_uncached += len;
    g_copy_calls++;
    if (len > g_copy_peak) g_copy_peak = len;
    if (!gva) g_copy_zero += len;
}

typedef struct GfxOut {
    u64 addr, frame;
    u32 size;
    VkBuffer buf;
    u64 off;
    VmaAllocation mem;
    u8* ptr;
    u8* org;
    u64* pages; /* sorted guest CPU pages behind this GPU virtual range */
    u32 npages;
    struct GfxOut* next;
} GfxOut;
static GfxOut *g_prep, *g_prep_tail, *g_out, *g_out_tail;
/* Suppress recursive lookups by the landing thread. Other readers still take vk_lock and wait for landing. */
static _Thread_local int g_out_landing;

void vk_buf_writable_abort(void) {
    while (g_prep) {
        GfxOut* o = g_prep;
        g_prep = o->next;
        free(o->org);
        free(o->pages);
        free(o);
    }
    g_prep_tail = NULL;
}
void vk_buf_writable_commit(void) {
    if (!g_prep) return;
    if (g_out_tail)
        g_out_tail->next = g_prep;
    else
        g_out = g_prep;
    g_out_tail = g_prep_tail;
    g_prep = g_prep_tail = NULL;
    g_gfx_any = 1;
}
/* The frame fence has signaled. Its stage allocations still exist here; cmd.c frees them only after this call. */
void vk_buf_frame_done(u64 frame) {
    GfxOut *done = NULL, *done_tail = NULL;
    for (GfxOut** pp = &g_out; *pp;) {
        GfxOut* o = *pp;
        if (o->frame != frame) {
            pp = &o->next;
            continue;
        }
        *pp = o->next;
        o->next = NULL;
        if (done_tail)
            done_tail->next = o;
        else
            done = o;
        done_tail = o;
    }
    g_out_tail = NULL;
    for (GfxOut* o = g_out; o; o = o->next) g_out_tail = o;
    g_gfx_any = g_out != NULL || done != NULL;
    VmaAllocation invalidated = VK_NULL_HANDLE;
    g_out_landing++;
    while (done) {
        GfxOut* o = done;
        done = o->next;
        if (o->mem != invalidated) {
            if (!VK_OK(vmaInvalidateAllocation(g_vk.vma, o->mem, 0, VK_WHOLE_SIZE)))
                fatal("Could not invalidate a graphics storage buffer after its frame completed");
            invalidated = o->mem;
        }
        vk_write_changed(o->addr, o->ptr, o->org, o->size);
        if (copy_stats_on()) {
            g_wb_outputs++;
            g_wb_bytes += o->size;
        }
        free(o->org);
        free(o->pages);
        free(o);
    }
    g_out_landing--;
    g_gfx_any = g_out != NULL;
}
void vk_buf_host_barrier(VkCommandBuffer cb, u64 frame) {
    for (GfxOut* o = g_out; o; o = o->next)
        if (o->frame == frame) {
            VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask =
                                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                  .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
            VkDependencyInfo d = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
            vkCmdPipelineBarrier2(cb, &d);
            return;
        }
}
static void wait_finish(int reason) {
    u64 t = copy_stats_on() ? plat_ns() : 0;
    vk_finish();
    if (t) {
        u64 ns = plat_ns() - t;
        g_wb_waits++;
        g_wb_wait_ns += ns;
        g_wb_reason[reason]++;
        g_wb_reason_ns[reason] += ns;
    }
}
static void land_reason(int reason) {
    if (!g_gfx_any || g_out_landing) return;
    vk_lock();
    if (g_gfx_any && !g_out_landing) wait_finish(reason);
    vk_unlock();
}
void vk_buf_land(void) { land_reason(WB_SIGNAL); }
void vk_buf_land_syncwait(void) { land_reason(WB_SYNCWAIT); }
void vk_buf_land_map(void) { land_reason(WB_MAP); }
/* Two GPU virtual ranges can name the same guest pages. Mapping changes land pending output while holding vk_lock,
   so the translations remain stable until this check and any following landing have completed. */
static int page_cmp(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return (x > y) - (x < y);
}
static int backing_overlap(u64 a, u64 an, const GfxOut* o) {
    for (u64 ap = a >> 12, ae = (a + an - 1) >> 12; ap <= ae; ap++) {
        u64 ac = gmmu_translate(ap << 12);
        if (!ac) continue;
        u64 page = ac >> 12;
        if (bsearch(&page, o->pages, o->npages, sizeof page, page_cmp)) return 1;
    }
    return 0;
}
static void mem_need_reason(u64 gva, u64 len, int reason) {
    if (!g_gfx_any || g_out_landing || !len) return;
    vk_lock();
    for (GfxOut* o = g_out; o; o = o->next)
        if ((gva < o->addr + o->size && o->addr < gva + len) || backing_overlap(gva, len, o)) {
            wait_finish(reason);
            break;
        }
    vk_unlock();
}
void vk_buf_mem_need(u64 gva, u64 len) { mem_need_reason(gva, len, WB_READ); }
void vk_buf_mem_need_map(u64 gva, u64 len) { mem_need_reason(gva, len, WB_MAP); }
void vk_buf_before_write(u64 gva, u64 len) {
    if (len)
        mem_need_reason(gva, len, WB_WRITE);
    else
        land_reason(WB_SIGNAL);
}
/* A draw can consume an earlier draw's storage result without making a round trip through guest RAM. The guest
   input must still match the first draw's snapshot, and the frame must own the output buffer until this draw ends. */
static int gfx_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (!g_gfx_any || g_out_landing || !gva || !len) return 0;
    if (g_cs_any) vk_compute_mem_need(gva, len);
    GfxOut* match = NULL;
    int virtual_match = 0;
    for (GfxOut* o = g_out; o; o = o->next) {
        int virtual_overlap = gva < o->addr + o->size && o->addr < gva + len;
        if (virtual_overlap || backing_overlap(gva, len, o)) {
            if (match) {
                /* Separate earlier draws can own different bytes of this range. No single buffer is complete. */
                land_reason(WB_DRAW);
                return 0;
            }
            match = o;
            virtual_match = virtual_overlap;
        }
    }
    if (!match) return 0;
    if (!virtual_match) {
        land_reason(WB_DRAW);
        return 0;
    }
    GfxOut* o = match;
    u64 delta = gva - o->addr;
    u8* host = gva >= o->addr && len <= o->size - delta ? gmem_ptr_untracked(gva, len) : NULL;
    if (o->frame == vk_frame_number() && host && !memcmp(host, o->org + delta, len) &&
        !((o->off + delta) & (vk_stage_align() - 1))) {
        static u64 barrier_frame = ~0ull, barrier_draw = ~0ull;
        if (barrier_frame != vk_frame_number() || barrier_draw != g_vk_drawn) {
            vk_render_end();
            VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask =
                                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
                                  .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
            VkDependencyInfo d = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
            vkCmdPipelineBarrier2(vk_cmd(), &d);
            barrier_frame = vk_frame_number();
            barrier_draw = g_vk_drawn;
        }
        *buf = o->buf;
        *off = o->off + delta;
        if (copy_stats_on()) g_wb_reuses++;
        return 1;
    }
    if (copy_stats_on())
        g_wb_draw_miss[o->frame != vk_frame_number() ? 0 : !host ? 1 : memcmp(host, o->org + delta, len) ? 2 : 3]++;
    land_reason(WB_DRAW);
    return 0;
}

/* written-page tracking (plat_ww_scan): a generation per 4K page of guest memory, bumped when a scan finds it
   written. A cache entry remembers the clock when it last matched memory, and is stale only if a page in its range
   has a newer generation. Scanning re-protects what it reports, and the shared table keeps that from hiding a change
   from another entry over the same memory. The table has a leaf of 1024 pages per 4 MB that has been written, with
   the newest generation in the leaf beside it so whole leaves are skipped */
#define WW_LEAF_SHIFT 22
static u32* g_wleaf[AS_SIZE >> WW_LEAF_SHIFT];
static u32 g_wleaf_max[AS_SIZE >> WW_LEAF_SHIFT];
static u32 g_wclock = 1;
static int g_ww = -1; /* -1 not tried yet, 0 not available, 1 on */
static void ww_mark(u8* p, u64 n) {
    u64 o = (u64)(p - g_mem);
    if (o >= AS_SIZE || !n) return;
    for (u64 pg = o >> 12, e = (o + n - 1) >> 12; pg <= e && pg < (AS_SIZE >> 12); pg++) {
        u64 l = pg >> (WW_LEAF_SHIFT - 12);
        if (!g_wleaf[l]) {
            /* a leaf without a table stands for all its pages at its newest generation (ww_mark_range, or a table
               that couldn't be allocated): a new table starts them there */
            u32* t = calloc(1u << (WW_LEAF_SHIFT - 12), 4);
            if (t && g_wleaf_max[l])
                for (u32 i = 0; i < 1u << (WW_LEAF_SHIFT - 12); i++) t[i] = g_wleaf_max[l];
            g_wleaf[l] = t;
        }
        if (g_wleaf[l]) g_wleaf[l][pg & ((1u << (WW_LEAF_SHIFT - 12)) - 1)] = g_wclock;
        g_wleaf_max[l] = g_wclock;
    }
}
/* the pages of guest memory [o, e) as written now. A leaf the range covers whole, and that has no table, only gets
   its newest generation: kernel/mem.c can unmap gigabytes at once */
static void ww_mark_range(u64 o, u64 e) {
    const u64 lp = 1ull << (WW_LEAF_SHIFT - 12);
    for (u64 pg = o >> 12, pe = (e + 4095) >> 12; pg < pe;) {
        u64 l = pg >> (WW_LEAF_SHIFT - 12), lend = (l + 1) * lp, end = pe < lend ? pe : lend;
        if (!g_wleaf[l] && pg == l * lp && end == lend)
            g_wleaf_max[l] = g_wclock;
        else
            ww_mark(g_mem + (pg << 12), (end - pg) << 12);
        pg = end;
    }
}

/* ranges of guest memory changed without a write the tracking sees (vk_mem_changed: decommitted pages, which read
   as zero now). Noted on the game's threads, outside vk_lock, and marked written by the next vk_written_since, as if
   its scan had found them. When the list is full its last range grows to cover the new one, marking more than
   changed: a cache over the rest only checks its contents once */
typedef struct {
    u64 a, e;
} WwRange;
#define WW_CHG_N 32
static WwRange g_wchg[WW_CHG_N];
static int g_wchg_n; /* atomic: read without the lock to skip it */
static PMutex g_wchg_lock = PMUTEX_INIT;
void vk_mem_changed(u64 cpu, u64 len) {
    if (cpu >= AS_SIZE || !len) return;
    u64 e = len > AS_SIZE - cpu ? AS_SIZE : cpu + len;
    pm_lock(&g_wchg_lock);
    int n = __atomic_load_n(&g_wchg_n, __ATOMIC_RELAXED);
    if (n < WW_CHG_N) {
        g_wchg[n] = (WwRange){cpu, e};
        __atomic_store_n(&g_wchg_n, n + 1, __ATOMIC_RELEASE);
    } else {
        WwRange* r = &g_wchg[WW_CHG_N - 1];
        if (cpu < r->a) r->a = cpu;
        if (e > r->e) r->e = e;
    }
    pm_unlock(&g_wchg_lock);
}
static void ww_take_changed(void) {
    if (!__atomic_load_n(&g_wchg_n, __ATOMIC_ACQUIRE)) return;
    WwRange c[WW_CHG_N];
    pm_lock(&g_wchg_lock);
    int n = __atomic_load_n(&g_wchg_n, __ATOMIC_RELAXED);
    memcpy(c, g_wchg, (size_t)n * sizeof *c);
    __atomic_store_n(&g_wchg_n, 0, __ATOMIC_RELAXED);
    pm_unlock(&g_wchg_lock);
    for (int i = 0; i < n; i++) ww_mark_range(c[i].a, c[i].e);
}

int vk_written_since(const u8* host, u64 len, u32 since) {
    if (!g_ww || !host || !len) return -1;
    if (g_ww < 0) {
        g_ww = plat_ww_scan(g_mem, 4096, ww_mark) == 0;
        LOG("vulkan: %s\n",
            g_ww ? "write tracking on (caches skip unchanged memory)" : "no write tracking (caches check contents)");
        if (!g_ww) return -1;
    }
    g_wclock++;
    ww_take_changed();
    if (plat_ww_scan((u8*)host, len, ww_mark) < 0) {
        g_ww = 0;
        LOG("vulkan: write tracking failed, caches check contents\n");
        return -1;
    }
    u64 o = (u64)(host - g_mem);
    for (u64 pg = o >> 12, e = (o + len - 1) >> 12; pg <= e;) {
        u64 l = pg >> (WW_LEAF_SHIFT - 12), lend = (l + 1) << (WW_LEAF_SHIFT - 12);
        if (g_wleaf_max[l] > since) {
            if (!g_wleaf[l]) return 1; /* the whole leaf was marked */
            for (u64 q = pg; q <= e && q < lend; q++)
                if (g_wleaf[l][q & ((1u << (WW_LEAF_SHIFT - 12)) - 1)] > since) return 1;
        }
        pg = lend;
    }
    return 0;
}
u32 vk_ww_now(void) { return g_wclock; }

static u8* g_tmp;
static u64 g_tmp_cap;
/* the guest bytes, without a copy when they are contiguous in host memory */
static const u8* guest_bytes(u64 gva, u64 len) {
    u8* p = gmem_ptr(gva, len);
    if (p) return p;
    if (g_tmp_cap < len) {
        u8* n = realloc(g_tmp, len);
        if (!n) return NULL;
        g_tmp = n;
        g_tmp_cap = len;
    }
    gmem_read(gva, g_tmp, len);
    return g_tmp;
}

int vk_buf_copy(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (gva && gfx_gpu_src(gva, len, buf, off)) return 1;
    u8* p = vk_stage(len ? len : 16, vk_stage_align(), buf, off);
    if (!p) return 0;
    if (!gva)
        memset(p, 0, len);
    else {
        const u8* src = guest_bytes(gva, len);
        if (!src) return 0;
        memcpy(p, src, len);
    }
    copy_note(gva, len);
    return 1;
}

/* Each writable binding keeps its original bytes until its draw's frame completes. The guest can keep writing RAM:
   vk_write_changed resolves those writes against the GPU result at the fence. */
int vk_buf_copy_writable(u64 gva, u32 len, VkBuffer* buf, u64* off) {
    if (!gva) return vk_buf_copy(0, len, buf, off);
    /* Repeated bindings of one writable range in the same draw must name the same storage. */
    for (GfxOut* p = g_prep; p; p = p->next)
        if (p->addr == gva && p->size == len) {
            *buf = p->buf;
            *off = p->off;
            return 1;
        }
    if (gfx_gpu_src(gva, len, buf, off)) return 1;
    GfxOut* o = calloc(1, sizeof *o);
    if (!o) return 0;
    o->org = malloc(len);
    if (!o->org) {
        free(o);
        return 0;
    }
    const u8* src = guest_bytes(gva, len);
    if (!src) {
        free(o->org);
        free(o);
        return 0;
    }
    memcpy(o->org, src, len);
    o->npages = (u32)(((gva & 0xFFF) + len + 0xFFF) >> 12);
    o->pages = calloc(o->npages, sizeof *o->pages);
    if (!o->pages) {
        free(o->org);
        free(o);
        return 0;
    }
    for (u32 i = 0; i < o->npages; i++) {
        u64 cpu = gmmu_translate((gva & ~0xFFFull) + (u64)i * 0x1000);
        o->pages[i] = cpu ? cpu >> 12 : ~0ull;
    }
    qsort(o->pages, o->npages, sizeof *o->pages, page_cmp);
    u64 align = vk_stage_align();
    if (align < g_vk.props.limits.nonCoherentAtomSize) align = g_vk.props.limits.nonCoherentAtomSize;
    o->ptr = vk_stage_output(len, align, buf, off, &o->mem);
    if (!o->ptr) {
        free(o->org);
        free(o->pages);
        free(o);
        return 0;
    }
    memcpy(o->ptr, o->org, len);
    o->addr = gva;
    o->size = len;
    o->buf = *buf;
    o->off = *off;
    o->frame = vk_frame_number();
    if (g_prep_tail)
        g_prep_tail->next = o;
    else
        g_prep = o;
    g_prep_tail = o;
    copy_note(gva, len);
    return 1;
}

typedef struct {
    u64 key, gva;
    int writable, bind;
} DrawPage;
static int draw_page_cmp(const void* a, const void* b) {
    u64 x = ((const DrawPage*)a)->key, y = ((const DrawPage*)b)->key;
    return (x > y) - (x < y);
}
static u64 draw_page_key(u64 gva) {
    u64 cpu = gmmu_translate(gva);
    /* Unmapped virtual pages are distinct and have no guest destination for writeback. */
    return cpu ? cpu >> 12 : (1ull << 63) | (gva >> 12);
}
static DrawPage* draw_pages(const VkDrawStorage* draw, size_t* count) {
    size_t n = 0;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < draw->n[s]; i++) {
            const VkDrawGbuf* b = &draw->b[s][i];
            if (b->addr) n += ((b->addr & 4095) + b->size + 4095) >> 12;
        }
    DrawPage* p = malloc((n ? n : 1) * sizeof *p);
    if (!p) return NULL;
    size_t k = 0;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < draw->n[s]; i++) {
            const VkDrawGbuf* b = &draw->b[s][i];
            if (!b->addr) continue;
            for (u64 g = b->addr & ~4095ull, e = b->addr + b->size; g < e; g += 4096)
                p[k++] = (DrawPage){draw_page_key(g), g, b->writable, s * 16 + i};
        }
    qsort(p, k, sizeof *p, draw_page_cmp);
    *count = k;
    return p;
}
static int draw_alias_bindings(const DrawPage* p, size_t n, u8 active[32]) {
    memset(active, 0, 32);
    int alias = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        int writable = p[i].writable;
        while (j < n && p[j].key == p[i].key) writable |= p[j++].writable;
        if (j - i > 1 && writable) {
            for (size_t k = i; k < j; k++) active[p[k].bind] = 1;
            alias = 1;
        }
        i = j;
    }
    return alias;
}
static int draw_has_writable(const VkDrawStorage* draw) {
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < draw->n[s]; i++)
            if (draw->b[s][i].addr && draw->b[s][i].writable) return 1;
    return 0;
}
int vk_buf_draw_has_alias(const VkDrawStorage* draw) {
    if (!draw_has_writable(draw)) return 0;
    size_t n;
    DrawPage* p = draw_pages(draw, &n);
    if (!p) return -1;
    u8 active[32];
    int alias = draw_alias_bindings(p, n, active);
    free(p);
    return alias;
}
static u32 draw_page_index(const DrawPage* p, size_t n, u64 key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (u32)lo;
}
static int draw_output_page(const DrawPage* p, u8* ptr, VkBuffer buf, u64 off, VmaAllocation mem) {
    if (p->key & (1ull << 63)) return 1;
    GfxOut* o = calloc(1, sizeof *o);
    if (!o) return 0;
    o->org = malloc(4096);
    o->pages = malloc(sizeof *o->pages);
    if (!o->org || !o->pages) {
        free(o->org);
        free(o->pages);
        free(o);
        return 0;
    }
    memcpy(o->org, ptr, 4096);
    o->pages[0] = p->key;
    o->npages = 1;
    o->addr = p->gva;
    o->size = 4096;
    o->frame = vk_frame_number();
    o->buf = buf;
    o->off = off;
    o->mem = mem;
    o->ptr = ptr;
    if (g_prep_tail)
        g_prep_tail->next = o;
    else
        g_prep = o;
    g_prep_tail = o;
    return 1;
}
/* All bindings of an aliasing draw index one page atlas. This also covers fragmented GPU mappings: the shader's
   word index is translated through its binding's page list, so a write through either VA reaches the same word. */
int vk_buf_draw_prepare(VkDrawStorage* draw) {
    size_t n = 0;
    int writable = draw_has_writable(draw);
    DrawPage* p = writable ? draw_pages(draw, &n) : NULL;
    if (writable && !p) return 0;
    u8 active[32] = {0};
    int alias = p ? draw_alias_bindings(p, n, active) : 0;
    size_t unique = 0;
    if (alias)
        for (size_t i = 0; i < n; i++)
            if (active[p[i].bind] && (!unique || p[i].key != p[unique - 1].key)) p[unique++] = p[i];
    VkBuffer atlas = VK_NULL_HANDLE;
    u64 atlas_off = 0, atlas_size = unique * 4096ull;
    VmaAllocation mem = VK_NULL_HANDLE;
    u8* ptr = NULL;
    if (alias) {
        if (atlas_size > g_vk.props.limits.maxStorageBufferRange) {
            WARN_LIMIT(1, "vulkan: aliased draw needs %llu storage bytes, beyond the device's descriptor range\n",
                       (unsigned long long)atlas_size);
            free(p);
            return 0;
        }
        u64 align = vk_stage_align();
        if (align < g_vk.props.limits.nonCoherentAtomSize) align = g_vk.props.limits.nonCoherentAtomSize;
        ptr = vk_stage_output(atlas_size, align, &atlas, &atlas_off, &mem);
        if (!ptr) {
            free(p);
            return 0;
        }
        for (size_t i = 0; i < unique; i++) {
            if (p[i].key & (1ull << 63))
                memset(ptr + i * 4096, 0, 4096);
            else
                gmem_read(p[i].gva, ptr + i * 4096, 4096);
        }
        copy_note(1, atlas_size);
    }
    for (int s = 0; s < 2; s++) {
        if (!draw->n[s]) continue;
        size_t words = 48;
        if (alias)
            for (int i = 0; i < draw->n[s]; i++) {
                VkDrawGbuf* b = &draw->b[s][i];
                if (b->addr && active[s * 16 + i]) words += ((b->addr & 4095) + b->size + 4095) >> 12;
            }
        u32* map = (u32*)vk_stage(words * 4, vk_stage_align(), &draw->map_buf[s], &draw->map_off[s]);
        if (!map) {
            free(p);
            return 0;
        }
        draw->map_size[s] = words * 4;
        for (int i = 0; i < 16; i++) map[i] = ~0u;
        for (int i = 16; i < 32; i++) map[i] = 0;
        for (int i = 32; i < 48; i++) map[i] = 0;
        size_t at = 48;
        if (alias)
            for (int i = 0; i < draw->n[s]; i++) {
                VkDrawGbuf* b = &draw->b[s][i];
                if (!b->addr || !active[s * 16 + i]) continue;
                map[i] = (u32)at;
                map[16 + i] = (u32)((b->addr & 4095) >> 2);
                map[32 + i] = b->size >> 2;
                for (u64 g = b->addr & ~4095ull, e = b->addr + b->size; g < e; g += 4096)
                    map[at++] = draw_page_index(p, unique, draw_page_key(g));
                b->aliased = 1;
                b->buf = atlas;
                b->off = atlas_off;
                b->range = atlas_size;
            }
    }
    if (alias)
        for (size_t i = 0; i < unique; i++)
            if (!draw_output_page(&p[i], ptr + i * 4096, atlas, atlas_off + i * 4096, mem)) {
                vk_buf_writable_abort();
                free(p);
                return 0;
            }
    if (alias && copy_stats_on()) {
        g_alias_draws++;
        g_alias_pages += unique;
    }
    free(p);
    return 1;
}

/* the entry's data where draws read it */
static void use(Vb* e, VkBuffer* buf, u64* off) {
    e->used = g_vk_present;
    if (copy_stats_on()) g_read_bound += e->len;
    *buf = e->buf;
    *off = e->off;
}
static void drop_own(Vb* e) {
    if (e->own) vk_defer_buffer(e->buf, e->mem);
    e->own = 0;
    e->buf = VK_NULL_HANDLE;
}
static int make_own(Vb* e, const u8* src) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = e->len,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo info;
    VkBuffer b;
    VmaAllocation m;
    if (vmaCreateBuffer(g_vk.vma, &bci, &aci, &b, &m, &info) != VK_SUCCESS) return 0;
    memcpy(info.pMappedData, src, e->len);
    if (copy_stats_on()) g_copy_owned += e->len;
    vmaFlushAllocation(g_vk.vma, m, 0, VK_WHOLE_SIZE);
    drop_own(e);
    e->own = 1;
    e->buf = b;
    e->mem = m;
    e->off = 0;
    return 1;
}

static int buf_get(u64 gva, u64 len, VkBuffer* buf, u64* off, int strict) {
    if (!len) return 0;
    if (gfx_gpu_src(gva, len, buf, off)) return 1;
    if (vk_compute_gpu_src(gva, len, buf, off)) return 1; /* a compute result that hasn't landed yet */
    /* one that covers the range only in part lands first (its write shows in gpu_mem_dirty_since below): the cached
       copy would otherwise be used without it */
    if (g_cs_any) vk_compute_mem_need(gva, len);
    u32 k = (u32)((gva >> 6) ^ (gva >> 20) ^ len) & (VBH - 1);
    Vb* e = g_vbh[k];
    for (; e; e = e->next)
        if (e->start == gva && e->len == len) break;
    /* valid: the data is where draws can read it this frame */
    int valid = e && e->buf && (e->own || e->frame == vk_frame_number());
    if (valid && !gpu_mem_dirty_since(e->gw, gva, len)) {
        if (e->epoch == g_vk_epoch && (!strict || e->strict_epoch == g_vk_epoch)) {
            use(e, buf, off);
            return 1;
        }
        /* with write tracking, an own buffer is checked only when its memory was written */
        int w = e->own ? vk_written_since(gmem_ptr(gva, len), len, e->wseen) : -1;
        if (w == 0) {
            e->epoch = g_vk_epoch;
            if (strict) e->strict_epoch = g_vk_epoch;
            use(e, buf, off);
            return 1;
        }
        /* without it: own buffers that stayed the same for long are checked every few submits, big ones sampled in
           between. A CPU write in between shows that many submits late; writes of the GPU front end always show at
           once (gpu_mem_dirty_since above) */
        if (!strict && w < 0 && e->own && e->stable >= 16 && len >= (64u << 10) && g_vk_epoch - e->chk_epoch < 8) {
            e->epoch = g_vk_epoch;
            use(e, buf, off);
            return 1;
        }
        if (!strict && w < 0 && e->own && e->stable >= 32 && len < (64u << 10) && g_vk_epoch - e->chk_epoch < 4) {
            e->epoch = g_vk_epoch;
            use(e, buf, off);
            return 1;
        }
        if (!strict && w < 0 && e->own && e->stable >= 64 && len >= 4096 &&
            g_vk_epoch - e->chk_epoch < (e->stable >= 256 ? 240u : 64u)) {
            if (g_vk_epoch - e->schk < 4 || vk_sample_hash(gva, len) == e->shash) {
                if (g_vk_epoch - e->schk >= 4) e->schk = g_vk_epoch;
                e->epoch = g_vk_epoch;
                use(e, buf, off);
                return 1;
            }
        }
    }
    u32 wnow = vk_ww_now();
    const u8* src = guest_bytes(gva, len);
    if (!src) return 0;
    u64 h = vk_hash(src, len);
    if (!e) {
        e = calloc(1, sizeof *e);
        if (!e) return 0;
        e->start = gva;
        e->len = len;
        e->hash = h ^ 1;
        e->next = g_vbh[k];
        g_vbh[k] = e;
    }
    if (e->hash == h && e->stable < 100000)
        e->stable++;
    else if (e->hash != h) {
        e->hash = h;
        e->stable = 0;
        drop_own(e);
    }
    e->epoch = e->chk_epoch = g_vk_epoch;
    if (strict) e->strict_epoch = g_vk_epoch;
    e->gw = gpu_mem_generation();
    e->wseen = wnow;
    if (e->stable >= 63 && len >= 4096) {
        e->shash = vk_sample_hash(gva, len);
        e->schk = g_vk_epoch;
    }
    if (!e->own && !(e->stable >= 2 && make_own(e, src))) {
        /* changing (or new) data: this frame's staging ring */
        e->buf = VK_NULL_HANDLE; /* a failed allocation must not leave the old snapshot reusable */
        u8* p = vk_stage(len, vk_stage_align(), &e->buf, &e->off);
        if (!p) return 0;
        memcpy(p, src, len);
        if (copy_stats_on()) g_copy_stage += len;
        e->frame = vk_frame_number();
    }
    use(e, buf, off);
    return 1;
}

int vk_buf_get(u64 gva, u64 len, VkBuffer* buf, u64* off) { return buf_get(gva, len, buf, off, 0); }
/* Constant buffers cannot accept the delayed checks used for stable vertex data: a changed word affects this draw. */
int vk_buf_get_strict(u64 gva, u64 len, VkBuffer* buf, u64* off) { return buf_get(gva, len, buf, off, 1); }

/* buffers not read for ten seconds go */
void vk_buf_frame(void) {
    if (g_vk_present % 60 || g_vk_present < 600) return;
    if (copy_stats_on() && g_vk_present % 600 == 0) {
        LOG("vulkan: buffer bytes over 600 presents: uncached=%llu zero=%llu calls=%llu peak=%llu staged=%llu owned=%llu bound=%llu writeback=%llu/%llu waits=%llu/%llums reused=%llu\n",
            (unsigned long long)g_copy_uncached, (unsigned long long)g_copy_zero, (unsigned long long)g_copy_calls,
            (unsigned long long)g_copy_peak, (unsigned long long)g_copy_stage, (unsigned long long)g_copy_owned,
            (unsigned long long)g_read_bound, (unsigned long long)g_wb_outputs, (unsigned long long)g_wb_bytes,
            (unsigned long long)g_wb_waits, (unsigned long long)(g_wb_wait_ns / 1000000),
            (unsigned long long)g_wb_reuses);
        LOG("vulkan: buffer waits: draw=%llu/%llums read=%llu/%llums write=%llu/%llums signal=%llu/%llums syncwait=%llu/%llums map=%llu/%llums draw_miss=frame:%llu range:%llu changed:%llu align:%llu\n",
            (unsigned long long)g_wb_reason[WB_DRAW], (unsigned long long)(g_wb_reason_ns[WB_DRAW] / 1000000),
            (unsigned long long)g_wb_reason[WB_READ], (unsigned long long)(g_wb_reason_ns[WB_READ] / 1000000),
            (unsigned long long)g_wb_reason[WB_WRITE], (unsigned long long)(g_wb_reason_ns[WB_WRITE] / 1000000),
            (unsigned long long)g_wb_reason[WB_SIGNAL], (unsigned long long)(g_wb_reason_ns[WB_SIGNAL] / 1000000),
            (unsigned long long)g_wb_reason[WB_SYNCWAIT], (unsigned long long)(g_wb_reason_ns[WB_SYNCWAIT] / 1000000),
            (unsigned long long)g_wb_reason[WB_MAP], (unsigned long long)(g_wb_reason_ns[WB_MAP] / 1000000),
            (unsigned long long)g_wb_draw_miss[0], (unsigned long long)g_wb_draw_miss[1],
            (unsigned long long)g_wb_draw_miss[2], (unsigned long long)g_wb_draw_miss[3]);
        LOG("vulkan: aliased storage draws=%llu pages=%llu\n", (unsigned long long)g_alias_draws,
            (unsigned long long)g_alias_pages);
        g_copy_uncached = g_copy_stage = g_copy_owned = g_copy_zero = g_copy_calls = g_copy_peak = g_read_bound = 0;
        g_wb_outputs = g_wb_bytes = g_wb_waits = g_wb_wait_ns = g_wb_reuses = 0;
        g_alias_draws = g_alias_pages = 0;
        memset(g_wb_reason, 0, sizeof g_wb_reason);
        memset(g_wb_reason_ns, 0, sizeof g_wb_reason_ns);
        memset(g_wb_draw_miss, 0, sizeof g_wb_draw_miss);
    }
    for (int k = 0; k < VBH; k++)
        for (Vb** pp = &g_vbh[k]; *pp;) {
            Vb* e = *pp;
            if ((i32)(e->used - (g_vk_present - 600)) >= 0) {
                pp = &e->next;
                continue;
            }
            *pp = e->next;
            drop_own(e);
            free(e);
        }
}
