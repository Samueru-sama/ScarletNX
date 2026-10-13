/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the memory SVCs (heap, QueryMemory, MapMemory, physical, shared and transfer memory) and GetInfo */
#include "hle/kernel/kernel_int.h"
#include "runtime/clock.h"
/* ============================================================ memory */
u32 svc_query(u64 info, u64 addr) {
    MemBlock b;
    mem_query(addr, &b);
    gw64(info, b.addr);
    gw64(info + 8, b.size);
    gw32(info + 16, b.state & 0xFF);
    gw32(info + 20, b.attr);
    gw32(info + 24, b.perm & 7);
    gw32(info + 28, b.ipc);
    gw32(info + 32, b.dev);
    gw32(info + 36, 0);
    return 0;
}
/* the console's argument checks: InvalidAddress for an unaligned address, InvalidSize for a size of 0 or one that
   isn't whole pages, InvalidCurrentMemory when the range wraps */
static u32 check_range(u64 a, u64 s) {
    if (a & 0xFFF) return R_BADADDR;
    if (!s || (s & 0xFFF)) return R_BADSIZE;
    if (a + s < a) return R_BADMEMSTATE;
    return 0;
}
/* every block of [a, a+s) is mapped, in state st (any state when ~0), with (attr & mask) == want */
static int range_is(u64 a, u64 s, u32 st, u32 mask, u32 want) {
    for (u64 p = a; p < a + s;) {
        MemBlock b;
        mem_query(p, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) return 0;
        if ((st != 0xFFFFFFFFu && b.state != st) || (b.attr & mask) != want) return 0;
        p = b.addr + b.size;
    }
    return 1;
}
u32 svc_map_memory(u64 dst, u64 src, u64 size) {
    u32 r = check_range(dst, size);
    if (!r) r = check_range(src, size);
    if (r) return r;
    int in_stack = dst >= STACK_BASE && dst + size <= STACK_BASE + STACK_SIZE;
    int in_alias = dst >= ALIAS_BASE && dst + size <= ALIAS_BASE + ALIAS_SIZE;
    if (!in_stack && !in_alias) return R_BADREGION;
    int m = mem_map(dst, size, in_stack ? MS_STACK : MS_NORMAL, PERM_RW);
    if (m) return m == -2 ? R_OUTOFMEM : R_BADMEMSTATE;
    memcpy(GP(dst), GP(src), size);
    mem_set_attr(src, size, 1, 1); /* locked */
    mem_set_state(src, size, 0xFFFFFFFFu, PERM_NONE);
    return 0;
}
u32 svc_unmap_memory(u64 dst, u64 src, u64 size) {
    u32 r = check_range(dst, size);
    if (!r) r = check_range(src, size);
    if (r) return r;
    int in_stack = dst >= STACK_BASE && dst + size <= STACK_BASE + STACK_SIZE;
    int in_alias = dst >= ALIAS_BASE && dst + size <= ALIAS_BASE + ALIAS_SIZE;
    if (!in_stack && !in_alias) return R_BADREGION;
    /* only what MapMemory mapped: the alias in the state it got, the source still locked */
    if (!range_is(dst, size, in_stack ? MS_STACK : MS_NORMAL, 0, 0) || !range_is(src, size, 0xFFFFFFFFu, 1, 1))
        return R_BADMEMSTATE;
    memcpy(GP(src), GP(dst), size);
    mem_unmap(dst, size);
    mem_set_attr(src, size, 1, 0);
    mem_set_state(src, size, 0xFFFFFFFFu, PERM_RW);
    return 0;
}
/* Single application process: its image, main stack and private system-resource reservation are charged before
   any heap or physical pages. Alias views of heap pages must not be charged a second time. */
static u64 process_memory(void) { return g_code_memory + g_main_stack_size; }
static u64 dynamic_budget(void) {
    u64 fixed = process_memory() + g_sysres_size, total = mem_total();
    return fixed < total ? total - fixed : 0;
}
static u32 memory_result(int r) {
    return r == MEM_LIMIT ? R_LIMIT : r == MEM_NOMEM ? R_OUTOFMEM : r ? R_BADMEMSTATE : 0;
}
/* svcSetHeapSize: validate the request here, but reserve/map/account under the memory lock shared with physical
   mappings. A fixed-size startup request need not retry after failure. */
u32 svc_set_heap_size(u64 size) {
    u32 r = 0;
    if ((size & 0x1FFFFF) || size >= 0x200000000ull)
        r = R_BADSIZE;
    else if (process_memory() > HEAP_SIZE || size > HEAP_SIZE - process_memory() || (g_heap_max && size > g_heap_max))
        r = R_OUTOFMEM;
    else
        r = memory_result(mem_heap_set(size, dynamic_budget()));
    LOG("heap: %llu MB requested%s (result %x)\n", (unsigned long long)(size >> 20), r ? ", refused" : "", r);
    return r;
}
/* Physical mapping needs the process's private system resource. Its overflow result differs from the other
   memory SVCs, so keep its argument checks separate. */
static u32 check_physical(u64 a, u64 s) {
    if (a & 0xFFF) return R_BADADDR;
    if (!s || (s & 0xFFF)) return R_BADSIZE;
    if (s > ~a) return R_BADREGION;
    if (!g_sysres_size) return R_BADSTATE;
    if (a < ALIAS_BASE || a + s > ALIAS_BASE + ALIAS_SIZE) return R_BADREGION;
    return 0;
}
u32 svc_map_physical(u64 a, u64 s) {
    u32 r = check_physical(a, s);
    return r ? r : memory_result(mem_map_physical(a, s, dynamic_budget()));
}
u32 svc_unmap_physical(u64 a, u64 s) {
    u32 r = check_physical(a, s);
    return r ? r : memory_result(mem_unmap_physical(a, s));
}
static void shm_destroy(KObj* o) {
    KShmem* s = (KShmem*)o;
    free(s->host); /* a mapping the game still has keeps its own copy in guest memory */
    free(o->waiters);
    free(s);
}
KShmem* kshmem_new(u64 size, const char* name) {
    KShmem* s = calloc(1, sizeof *s);
    s->o.type = KO_SHMEM;
    s->o.destroy = shm_destroy;
    s->o.name = name;
    s->size = (size + 0xFFF) & ~0xFFFull;
    s->host = calloc(1, s->size);
    return s;
}
/* the services write through this: the guest mapping once the game mapped it, the host copy before and after */
u8* kshmem_ptr(KShmem* s) {
    u64 g = __atomic_load_n(&s->gaddr, __ATOMIC_ACQUIRE);
    return g ? GP(g) : s->host;
}
u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm) {
    u32 r = check_range(addr, size);
    if (r) return r;
    KShmem* s = (KShmem*)h_get_ref(h, KO_SHMEM);
    if (!s) return R_BADHANDLE;
    if (size != s->size) { /* the console answers InvalidSize: keep mapping at least the whole object, but say so */
        WARN_LIMIT(1, "MapSharedMemory(%s) with size %llx, the object has %llx\n", s->o.name, (unsigned long long)size,
                   (unsigned long long)s->size);
        if (size < s->size) size = s->size;
    }
    if (mem_map(addr, size, MS_SHARED, perm & 3))
        r = R_BADMEMSTATE;
    else { /* under g_klock so the switch from the host copy happens in one step for kshmem_ptr's readers */
        pm_lock(&g_klock);
        memcpy(GP(addr), s->host, s->size);
        s->gsize = size;
        __atomic_store_n(&s->gaddr, addr, __ATOMIC_RELEASE);
        pm_unlock(&g_klock);
    }
    kobj_unref(&s->o);
    return r;
}
u32 svc_unmap_shm(u32 h, u64 addr, u64 size) {
    u32 r = check_range(addr, size);
    if (r) return r;
    KShmem* s = (KShmem*)h_get_ref(h, KO_SHMEM);
    if (!s) return R_BADHANDLE;
    pm_lock(&g_klock);
    u64 g = s->gaddr, gs = s->gsize;
    if (g == addr) {
        memcpy(s->host, GP(addr), s->size);
        __atomic_store_n(&s->gaddr, 0, __ATOMIC_RELEASE);
    }
    pm_unlock(&g_klock);
    if (g == addr)
        mem_unmap(addr, gs); /* what MapSharedMemory mapped */
    else
        r = R_BADMEMSTATE; /* not where this object is mapped */
    kobj_unref(&s->o);
    return r;
}
/* svcCreateTransferMemory locks the source with the permission it gives, closing the handle unlocks it */
void tmem_destroy(KObj* o) {
    KTmem* t = (KTmem*)o;
    mem_set_attr(t->addr, t->size, 1, 0);
    mem_set_state(t->addr, t->size, 0xFFFFFFFFu, PERM_RW);
    free(o->waiters);
    free(o);
}

/* ============================================================ GetInfo */
/* Application pools for the retail 4/6/8 GB arrangements. This runtime models one application, not a shared
   resource limit or a default application system-resource pool. See docs/MEMORY_ACCOUNTING.md. */
int g_opt_mem_gb;
u64 mem_total(void) {
    if (g_opt_mem_gb >= 8) return 6964ull << 20;
    if (g_opt_mem_gb >= 6) return 4916ull << 20;
    return 3285ull << 20;
}
static u64 used_non_system_memory(void) {
    return process_memory() + __atomic_load_n(&g_heap_size, __ATOMIC_SEQ_CST) +
           __atomic_load_n(&g_phys_mapped, __ATOMIC_SEQ_CST);
}
u64 used_memory(void) { return used_non_system_memory() + g_sysres_size; }
void mem_report(char* out, int n) {
    snprintf(out, n, "heap %llu MB, mapped physical memory %llu MB, used %llu MB of %llu MB",
             (unsigned long long)(__atomic_load_n(&g_heap_size, __ATOMIC_SEQ_CST) >> 20),
             (unsigned long long)(__atomic_load_n(&g_phys_mapped, __ATOMIC_SEQ_CST) >> 20),
             (unsigned long long)(used_memory() >> 20), (unsigned long long)(mem_total() >> 20));
}
/* the npdm's ThreadInfo capability sets them (main.c). These are the values before it was read */
u64 g_core_mask = 0xF, g_prio_mask = 0xFFFFFFFFFFFFFFF0ull;
static u32 svc_get_info_(u32 id0, u32 h, u64 id1, u64* out);
u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out) {
    u32 r = svc_get_info_(id0, h, id1, out);
    static u8 seen[64];
    if (id0 < 64 && !seen[id0]) {
        seen[id0] = 1;
        LOG("GetInfo %u/%llu -> %llx\n", id0, (unsigned long long)id1, (unsigned long long)*out);
    }
    return r;
}
static u32 svc_get_info_(u32 id0, u32 h, u64 id1, u64* out) {
    (void)h;
    *out = 0;
    switch (id0) {
        case 0: *out = g_core_mask; return 0;
        case 1: *out = g_prio_mask; return 0;
        case 2: *out = ALIAS_BASE; return 0;
        case 3: *out = ALIAS_SIZE; return 0;
        case 4: *out = HEAP_BASE; return 0;
        case 5: *out = HEAP_SIZE; return 0;
        case 6: *out = mem_total(); return 0; /* total app memory */
        case 7: *out = used_memory(); return 0;
        case 8: *out = 0; return 0;
        case 9: *out = 0; return 0; /* resource limit handle 0 */
        case 10: *out = 0; return 0;
        case 11: { /* RandomEntropy: four values the process gets when it starts, the same on every call */
            static u64 ent[4];
            if (id1 >= 4) return R_BADCOMBO;
            pm_lock(&g_klock);
            if (!ent[0]) {
                u64 x = plat_ns();
                for (int i = 0; i < 4; i++) { /* splitmix64 */
                    u64 z = (x += 0x9E3779B97F4A7C15ull);
                    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                    ent[i] = (z ^ (z >> 31)) | (i == 0); /* ent[0] != 0 marks them made */
                }
            }
            *out = ent[id1];
            pm_unlock(&g_klock);
            return 0;
        }
        case 12: *out = AS_BASE; return 0;
        case 13: *out = AS_SIZE - AS_BASE; return 0;
        case 14: *out = STACK_BASE; return 0;
        case 15: *out = STACK_SIZE; return 0;
        case 16: *out = g_sysres_size; return 0; /* SystemResourceSize (npdm) */
        case 17: *out = g_sysres_size ? 0x80000ull : 0; return 0;
        case 18: *out = g_title_id; return 0;
        case 19: *out = 0; return 0;
        case 20: { /* UserExceptionContextAddress: one page, made by the first call */
            static u64 uectx;
            pm_lock(&g_klock);
            if (!uectx) uectx = kmem_alloc(0x1000, MS_TLS, PERM_RW);
            *out = uectx;
            pm_unlock(&g_klock);
            return 0;
        }
        case 21: *out = mem_total() > g_sysres_size ? mem_total() - g_sysres_size : 0; return 0;
        case 22: *out = used_non_system_memory(); return 0;
        case 23: *out = 1; return 0;
        case 24: { /* FreeThreadCount: Unity skips its job threads if this is 0 */
            int live = kthread_live_count();
            *out = (u64)(live < THREAD_MAX ? THREAD_MAX - live : 0);
            return 0;
        }
        case 25:
        case 0xF0000002: /* ThreadTickCount. Guest threads aren't scheduled by us, so this is the system tick */
            if (id1 != ~0ull && id1 >= 4) return R_BADCOMBO;
            *out = guest_ticks();
            return 0;
        case 26: *out = 1; return 0;
        case 27:
        case 28: *out = 0; return 0;
        default: WARN("GetInfo %u/%llu not implemented\n", id0, (unsigned long long)id1); return R_BADENUM;
    }
}
