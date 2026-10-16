/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Releases the game can observe: a semaphore or report written for the CPU to poll, a syncpoint incremented. The
   console's GPU makes everything before a release visible in memory first, so storage results recorded before it
   land before it happens. Landing them with a vk_finish at each release left the GPU thread waiting for most of
   TotK's frame. A release now waits in a queue instead, gated on the submit that holds the work before it, and the
   GPU thread goes on recording. The release thread waits for that submit on the GPU, lands its results and performs
   the releases it gates, in order. Whoever needs a queued release sooner (a stream that reads the semaphore, or writes
   over it) performs it itself.
   The compute queue's results are waited for the same way (its batches, put aside, land in order). Storage images
   still land at the release (vk_gfx_image_land). */
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

typedef struct {
    u64 gate;  /* the graphics submit whose completion it waits for */
    u64 cgate; /* and the compute batch, 0 for none */
    u64 gva;   /* the payload's address, or 0 for a syncpoint increment */
    u32 id, len;
    u8 data[16];
} Release;
#define RQ_N   1024 /* queued releases at most: a full queue performs its oldest at once */
#define SYNC_N 128  /* nvdrv's syncpoint ids */

static Release g_rq[RQ_N];
static u64 g_rhead, g_rtail; /* releases queued, performed */
static u32 g_rincr[SYNC_N];  /* increments queued per syncpoint */
static PMutex g_rlock = PMUTEX_INIT;
static PCond g_rcv;
static int g_rthread;
static _Thread_local int t_performing; /* a release's own write doesn't wait for the queue */

static void perform(const Release* r) {
    t_performing = 1;
    if (r->gva)
        gmem_write(r->gva, r->data, r->len);
    else
        syncpt_incr(r->id);
    t_performing = 0;
}
/* lands the results of every submit up to gate and compute batch up to cgate (they have finished) and performs the
   releases they gate, in order */
static void release_through(u64 gate, u64 cgate) {
    vk_lock();
    vk_compute_land_through(cgate);
    vk_land_through(gate);
    for (;;) {
        Release r;
        pm_lock(&g_rlock);
        if (g_rtail == g_rhead || g_rq[g_rtail % RQ_N].gate > gate || g_rq[g_rtail % RQ_N].cgate > cgate) {
            pm_unlock(&g_rlock);
            break;
        }
        r = g_rq[g_rtail % RQ_N];
        g_rtail++;
        if (!r.gva && r.id < SYNC_N) g_rincr[r.id]--;
        if (g_rtail == g_rhead) __atomic_store_n(&g_gpu_releases, 0, __ATOMIC_RELEASE);
        pm_unlock(&g_rlock);
        perform(&r);
    }
    vk_unlock();
}
static void release_thread(void* p) {
    (void)p;
    for (;;) {
        u64 gate, cgate;
        pm_lock(&g_rlock);
        while (g_rtail == g_rhead) pc_wait(&g_rcv, &g_rlock, -1);
        gate = g_rq[g_rtail % RQ_N].gate;
        cgate = g_rq[g_rtail % RQ_N].cgate;
        pm_unlock(&g_rlock);
        vk_wait_submit(gate);
        vk_compute_wait(cgate);
        release_through(gate, cgate);
    }
}

/* r once everything recorded before it has landed: now, when nothing is pending or queued */
static void release(Release* r) {
    int queued;
    vk_lock();
    vk_gfx_image_land();
    r->cgate = vk_compute_put_aside();
    pm_lock(&g_rlock);
    queued = g_rtail != g_rhead;
    pm_unlock(&g_rlock);
    if (!vk_buf_pending_any() && !r->cgate && !queued) {
        vk_unlock();
        perform(r);
        return;
    }
    /* the work before it, submitted so its completion can be waited for; results of earlier submits are older */
    if (vk_buf_pending_unsubmitted()) vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    r->gate = vk_last_submit();
    pm_lock(&g_rlock);
    if (g_rhead - g_rtail == RQ_N) { /* full: the oldest goes now */
        u64 oldest = g_rq[g_rtail % RQ_N].gate, coldest = g_rq[g_rtail % RQ_N].cgate;
        pm_unlock(&g_rlock);
        vk_wait_submit(oldest);
        vk_compute_wait(coldest);
        release_through(oldest, coldest);
        pm_lock(&g_rlock);
    }
    g_rq[g_rhead % RQ_N] = *r;
    g_rhead++;
    if (!r->gva && r->id < SYNC_N) g_rincr[r->id]++;
    __atomic_store_n(&g_gpu_releases, 1, __ATOMIC_RELEASE);
    if (!g_rthread) {
        pc_init(&g_rcv);
        g_rthread = 1;
        if (plat_thread(release_thread, NULL, (size_t)1 << 20)) fatal("could not start the GPU release thread");
    }
    pc_broadcast(&g_rcv);
    pm_unlock(&g_rlock);
    vk_unlock();
}

void vk_release_write(u64 gva, const void* data, u32 len) {
    Release r;
    memset(&r, 0, sizeof r);
    r.gva = gva;
    r.len = len < sizeof r.data ? len : (u32)sizeof r.data;
    memcpy(r.data, data, r.len);
    release(&r);
}
void vk_release_incr(u32 id) {
    Release r;
    memset(&r, 0, sizeof r);
    r.id = id;
    release(&r);
}
/* the stream reads or writes [gva, gva + len): the queued releases up to the last one written there happen first */
void vk_release_need(u64 gva, u64 len) {
    u64 gate = 0, cgate = 0, i;
    int found = 0;
    if (t_performing) return;
    pm_lock(&g_rlock);
    for (i = g_rtail; i < g_rhead; i++) {
        const Release* r = &g_rq[i % RQ_N];
        if (r->gva && r->gva < gva + len && gva < r->gva + r->len) {
            gate = r->gate;
            cgate = r->cgate;
            found = 1;
        }
    }
    pm_unlock(&g_rlock);
    if (!found) return;
    vk_wait_submit(gate);
    vk_compute_wait(cgate);
    release_through(gate, cgate);
}
u32 vk_release_queued(u32 id) {
    u32 n;
    if (id >= SYNC_N) return 0;
    pm_lock(&g_rlock);
    n = g_rincr[id];
    pm_unlock(&g_rlock);
    return n;
}
