/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Priority inheritance in the kernel's mutex arbitration (sync.c), as the console's kernel does it: a mutex's owner
   runs at the best priority among its waiters, up a chain of owners that wait on mutexes themselves, until the mutex
   is handed over. The rest of the kernel is stubbed: a wait returns at once with the thread still queued, so each
   step's priorities can be read */
#include "hle/kernel/sync.c"
#include <stdarg.h>
#include <stdio.h>

u8* g_mem;
int g_verbose;
PMutex g_klock;
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}
u64 plat_ns(void) { return 0; }
void kobj_ref(KObj* o) { (void)o; }
void kobj_unref(KObj* o) { (void)o; }
void waiter_add(KObj* o, KThread* t) {
    (void)o;
    (void)t;
}
void waiter_del(KObj* o, KThread* t) {
    (void)o;
    (void)t;
}
int sleep_nl(KThread* t, i64 timeout) {
    (void)t;
    (void)timeout;
    return 1;
}
void wake(KThread* t, int result) {
    t->woken = 1;
    t->wait_result = result;
}
void core_set_prio(KThread* t, int prio) { t->prio = prio; }

enum { SNX_KMT_THREADS = 6, SNX_KMT_HANDLE = 0x100 };
static KThread snx_kmt_thread[SNX_KMT_THREADS];
KObj* h_get_nl(u32 h) {
    u32 i = h - SNX_KMT_HANDLE;
    return i < SNX_KMT_THREADS ? &snx_kmt_thread[i].o : NULL;
}
static u32 handle(KThread* t) { return SNX_KMT_HANDLE + (u32)(t - snx_kmt_thread); }

static unsigned checks, failures;
#define CHECK(expr, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "kernel_mutex.c:%d: %s: ", __LINE__, #expr);                                               \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

/* fresh threads at these base priorities, nobody waiting */
static KThread* reset(const int* prio) {
    memset(snx_kmt_thread, 0, sizeof snx_kmt_thread);
    memset(g_mem, 0, 0x1000);
    g_mwait = NULL;
    for (int i = 0; i < SNX_KMT_THREADS; i++) {
        snx_kmt_thread[i].o.type = KO_THREAD;
        snx_kmt_thread[i].prio = snx_kmt_thread[i].base_prio = prio[i];
    }
    return snx_kmt_thread;
}
static u32 word(u64 addr) { return *(u32*)(void*)(g_mem + addr); }
/* t blocks on the mutex at addr, which owner holds: the word names owner with the waiters bit, as the SDK leaves it
   before it asks the kernel */
static void lock(KThread* t, KThread* owner, u64 addr) {
    u32 v = handle(owner) | HANDLE_WAIT;
    memcpy(g_mem + addr, &v, 4);
    CHECK(svc_arbitrate_lock(t, handle(owner), addr, handle(t)) == 0, "lock");
}

int main(void) {
    g_mem = calloc(1, 0x1000);
    if (!g_mem) return 1;
    enum { SNX_KMT_M = 0x100, SNX_KMT_M2 = 0x200, SNX_KMT_CV = 0x300 };

    /* the owner, at 46, takes the priority of a waiter at 44 while it holds the mutex, and gives it back when it
       hands the mutex over */
    KThread* t = reset((const int[]){46, 44, 45, 40, 50, 30});
    KThread *a = &t[0], *b = &t[1];
    lock(b, a, SNX_KMT_M);
    CHECK(a->prio == 44 && a->base_prio == 46, "owner at %d (base %d) with a waiter at 44", a->prio, a->base_prio);
    CHECK(svc_arbitrate_unlock(SNX_KMT_M) == 0 && b->woken && word(SNX_KMT_M) == handle(b),
          "handed to the waiter: word %x", word(SNX_KMT_M));
    CHECK(a->prio == 46, "owner back at %d after the handover", a->prio);

    /* of two waiters the best takes the mutex, and the other then waits for it: its priority goes with it */
    t = reset((const int[]){46, 44, 45, 40, 50, 30});
    a = &t[0], b = &t[1];
    KThread* e = &t[3];
    lock(b, a, SNX_KMT_M);
    lock(e, a, SNX_KMT_M);
    CHECK(a->prio == 40, "owner at %d with waiters at 44 and 40", a->prio);
    svc_arbitrate_unlock(SNX_KMT_M);
    CHECK(e->woken && !b->woken && word(SNX_KMT_M) == (handle(e) | HANDLE_WAIT), "40 takes it, 44 still waits: word %x",
          word(SNX_KMT_M));
    CHECK(b->lock_owner == e && a->prio == 46 && e->prio == 40, "44 now waits for 40; old owner at %d", a->prio);

    /* a chain: the owner waits on a mutex of its own, so its owner inherits too */
    t = reset((const int[]){46, 44, 45, 40, 50, 30});
    a = &t[0], b = &t[1];
    KThread* d = &t[4];
    lock(a, d, SNX_KMT_M2);
    CHECK(d->prio == 46, "the second owner at %d with the first owner (46) waiting", d->prio);
    lock(b, a, SNX_KMT_M);
    CHECK(a->prio == 44 && d->prio == 44, "44 reaches both owners: %d and %d", a->prio, d->prio);

    /* SetThreadPriority on an owner moves its base, not what it inherits */
    t = reset((const int[]){46, 44, 45, 40, 50, 30});
    a = &t[0], b = &t[1];
    lock(b, a, SNX_KMT_M);
    prio_set_base_nl(a, 50);
    CHECK(a->prio == 44 && a->base_prio == 50, "base 50 under a waiter at 44: runs at %d", a->prio);
    svc_arbitrate_unlock(SNX_KMT_M);
    CHECK(a->prio == 50, "at its new base after the handover: %d", a->prio);

    /* a condition variable signalled while another thread holds the mutex: the woken waiter queues on the mutex and
       its owner inherits its priority */
    t = reset((const int[]){46, 44, 45, 40, 50, 30});
    a = &t[0], b = &t[1];
    u32 mine = handle(b);
    memcpy(g_mem + SNX_KMT_M, &mine, 4);
    CHECK(svc_wait_cv(b, SNX_KMT_M, SNX_KMT_CV, handle(b), -1) == 0, "wait on the condition variable");
    u32 held = handle(a);
    memcpy(g_mem + SNX_KMT_M, &held, 4);
    svc_signal_cv(SNX_KMT_CV, 1);
    CHECK(b->wait_kind == 2 && b->lock_owner == a && a->prio == 44, "signalled into a held mutex: owner at %d",
          a->prio);

    printf("%u checks, %u failed\n", checks, failures);
    free(g_mem);
    return failures != 0;
}
