/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Guest address space. One big host reservation, guest address == offset into it.
   The block list (kept sorted) says what is mapped where, in which state: mapping checks it, svcQueryMemory reads it. */
#include "memory/mem.h"
#include "gpu/gpu.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

u8* g_mem;
u64 g_heap_size, g_heap_max, g_phys_mapped;
u64 g_code_memory, g_main_stack_size;
static MemBlock* blk;
static int nblk, capblk;
static PMutex g_memlock;
/* the thread holding g_memlock. A crash inside mem.c is reported from the same thread, and the report's mem_query
   must not wait for a lock that thread will never release */
static __thread int t_memlock_held;
static void memlock(void) {
    pm_lock(&g_memlock);
    t_memlock_held = 1;
}
static void memunlock(void) {
    t_memlock_held = 0;
    pm_unlock(&g_memlock);
}

void mem_init(void) {
    pm_init(&g_memlock);
    g_mem = plat_vm_reserve(AS_SIZE);
    if (!g_mem) fatal("Could not reserve %llu GB of virtual memory for the game.", (unsigned long long)(AS_SIZE >> 30));
    /* page 0 is mapped and zeroed so a NULL(+off) read gives 0 instead of a crash */
    plat_vm_commit(g_mem, 0x10000);
}

static int reserve_blocks(int extra) {
    if (extra < 0 || extra > INT_MAX - nblk) return 0;
    int need = nblk + extra;
    if (need <= capblk) return 1;
    int cap = need < 256 ? 256 : need;
    if (capblk <= INT_MAX / 2 && cap < capblk * 2) cap = capblk * 2;
    MemBlock* p = realloc(blk, sizeof *blk * (size_t)cap);
    if (!p) return 0;
    blk = p;
    capblk = cap;
    return 1;
}
static void ins_at(int i, MemBlock b) {
    if (!reserve_blocks(1)) fatal("Out of memory tracking guest mappings");
    memmove(blk + i + 1, blk + i, sizeof(MemBlock) * (nblk - i));
    blk[i] = b;
    nblk++;
}
static void del_at(int i) {
    memmove(blk + i, blk + i + 1, sizeof(MemBlock) * (nblk - i - 1));
    nblk--;
}

/* split the block that contains a so that a block starts at a */
static void split_at(u64 a) {
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr < a && a < b->addr + b->size) {
            MemBlock n = *b;
            n.addr = a;
            n.size = b->addr + b->size - a;
            b->size = a - b->addr;
            ins_at(i + 1, n);
            return;
        }
    }
}
static void merge(void) {
    for (int i = 0; i + 1 < nblk;) {
        MemBlock *a = &blk[i], *b = &blk[i + 1];
        if (a->addr + a->size == b->addr && a->state == b->state && a->perm == b->perm && a->attr == b->attr &&
            a->ipc == b->ipc && a->dev == b->dev) {
            a->size += b->size;
            del_at(i + 1);
        } else
            i++;
    }
}
/* [a, a+s) lies in the game's address space (page 0 below AS_BASE is the runtime's) */
static int in_as(u64 a, u64 s) { return a >= AS_BASE && a + s <= AS_SIZE && a + s >= a; }

static int is_free_nl(u64 a, u64 s) {
    if (!in_as(a, s)) return 0;
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr < a + s && a < b->addr + b->size) return 0;
    }
    return 1;
}
static void ins_sorted_nl(MemBlock b) {
    int i = 0;
    while (i < nblk && blk[i].addr < b.addr) i++;
    ins_at(i, b);
    merge();
}
static int map_nl(u64 a, u64 s, u32 state, u32 perm) {
    if (!is_free_nl(a, s)) return MEM_BADSTATE;
    if (!reserve_blocks(1) || plat_vm_commit(GP(a), s)) return MEM_NOMEM;
    ins_sorted_nl((MemBlock){a, s, state, perm, 0, 0, 0});
    return 0;
}

int mem_map(u64 a, u64 s, u32 state, u32 perm) {
    if (!s) return 0;
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(0, 0, 1);
    memlock();
    int r = map_nl(a, s, state, perm);
    memunlock();
    if (g_gpu_mapping_end) g_gpu_mapping_end();
    return r;
}

/* Charge heap and alias pages together. The caller holds g_memlock, including through the host mapping. */
static int budget_fits(u64 heap, u64 physical, u64 budget) { return heap <= budget && physical <= budget - heap; }

/* Commit only holes, charging each page once. Delay the block-list updates so a failed host commit can undo
   only this call's new pages without touching pre-existing mappings, including adjacent ones. */
int mem_map_physical(u64 a, u64 s, u64 budget) {
    if (!in_as(a, s)) return MEM_BADSTATE;
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(0, 0, 1);
    memlock();
    u64 end = a + s, p = a, added = 0;
    int count = 0;
    MemBlock* holes = calloc((size_t)nblk + 1, sizeof *holes);
    if (!holes) {
        memunlock();
        if (g_gpu_mapping_end) g_gpu_mapping_end();
        return MEM_NOMEM;
    }
    for (int i = 0; i < nblk && p < end; i++) {
        MemBlock* b = &blk[i];
        if (b->addr + b->size <= p) continue;
        u64 e = b->addr < end ? b->addr : end;
        if (e > p) {
            holes[count++] = (MemBlock){p, e - p, MS_NORMAL, PERM_RW, 0, 0, 0};
            added += e - p;
        }
        p = b->addr + b->size;
    }
    if (p < end) {
        holes[count++] = (MemBlock){p, end - p, MS_NORMAL, PERM_RW, 0, 0, 0};
        added += end - p;
    }
    int r = 0;
    if (added && (!budget_fits(g_heap_size, g_phys_mapped, budget) || added > budget - g_heap_size - g_phys_mapped))
        r = MEM_LIMIT;
    else if (!reserve_blocks(count))
        r = MEM_NOMEM;
    int committed = 0;
    while (!r && committed < count) {
        MemBlock* h = &holes[committed];
        if (plat_vm_commit(GP(h->addr), h->size))
            r = MEM_NOMEM;
        else
            committed++;
    }
    if (r) {
        for (int i = 0; i < committed; i++) {
            plat_vm_decommit(GP(holes[i].addr), holes[i].size);
            gpu_mem_changed(holes[i].addr, holes[i].size);
        }
    } else {
        for (int i = 0; i < count; i++) ins_sorted_nl(holes[i]);
        __atomic_add_fetch(&g_phys_mapped, added, __ATOMIC_SEQ_CST);
    }
    free(holes);
    memunlock();
    if (g_gpu_mapping_end) g_gpu_mapping_end();
    return r;
}

static void unmap_nl(u64 a, u64 s) {
    split_at(a);
    split_at(a + s);
    for (int i = 0; i < nblk;) {
        MemBlock* b = &blk[i];
        if (b->addr >= a && b->addr + b->size <= a + s)
            del_at(i);
        else
            i++;
    }
    plat_vm_decommit(GP(a), s);
    /* the pages read as zero now, and no write the GPU caches' write tracking sees zeroed them */
    gpu_mem_changed(a, s);
}

int mem_unmap(u64 a, u64 s) {
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    if (!in_as(a, s)) return MEM_BADSTATE;
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(0, 0, 1);
    memlock();
    unmap_nl(a, s);
    memunlock();
    if (g_gpu_mapping_end) g_gpu_mapping_end();
    return 0;
}

int mem_unmap_physical(u64 a, u64 s) {
    if (!in_as(a, s)) return MEM_BADSTATE;
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(0, 0, 1);
    memlock();
    u64 n = 0;
    int r = 0;
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr >= a + s || b->addr + b->size <= a) continue;
        if (b->state != MS_NORMAL || b->attr) {
            r = MEM_BADSTATE;
            break;
        }
        u64 lo = b->addr > a ? b->addr : a;
        u64 hi = b->addr + b->size < a + s ? b->addr + b->size : a + s;
        n += hi - lo;
    }
    if (!r && n > g_phys_mapped) r = MEM_BADSTATE;
    if (!r && !reserve_blocks(2)) r = MEM_NOMEM;
    if (!r && n) {
        unmap_nl(a, s);
        __atomic_sub_fetch(&g_phys_mapped, n, __ATOMIC_SEQ_CST);
    }
    memunlock();
    if (g_gpu_mapping_end) g_gpu_mapping_end();
    return r;
}

int mem_set_state(u64 a, u64 s, u32 state, u32 perm) {
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    if (!in_as(a, s)) return -1;
    memlock();
    split_at(a);
    split_at(a + s);
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr >= a && b->addr + b->size <= a + s) {
            if (state != 0xFFFFFFFFu) b->state = state;
            if (perm != 0xFFFFFFFFu) b->perm = perm;
        }
    }
    merge();
    memunlock();
    return 0;
}

int mem_set_attr(u64 a, u64 s, u32 mask, u32 attr) {
    a &= ~0xFFFull;
    s = (s + 0xFFF) & ~0xFFFull;
    if (!in_as(a, s)) return -1;
    memlock();
    split_at(a);
    split_at(a + s);
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr >= a && b->addr + b->size <= a + s) b->attr = (b->attr & ~mask) | (attr & mask);
    }
    merge();
    memunlock();
    return 0;
}

void mem_query(u64 a, MemBlock* o) {
    int held = t_memlock_held; /* only when crashing inside mem.c */
    if (!held) memlock();
    if (a >= AS_SIZE) {
        o->addr = AS_SIZE;
        o->size = 0 - AS_SIZE;
        o->state = MS_INACCESSIBLE;
        o->perm = 0;
        o->attr = o->ipc = o->dev = 0;
        if (!held) memunlock();
        return;
    }
    u64 lo = 0, hi = AS_SIZE;
    for (int i = 0; i < nblk; i++) {
        MemBlock* b = &blk[i];
        if (b->addr <= a && a < b->addr + b->size) {
            *o = *b;
            if (!held) memunlock();
            return;
        }
        if (b->addr + b->size <= a && b->addr + b->size > lo) lo = b->addr + b->size;
        if (b->addr > a && b->addr < hi) hi = b->addr;
    }
    if (a < AS_BASE) {
        hi = hi < AS_BASE ? hi : AS_BASE;
        o->addr = 0;
        o->size = hi;
        o->state = MS_INACCESSIBLE;
        o->perm = 0;
        o->attr = o->ipc = o->dev = 0;
        if (!held) memunlock();
        return;
    }
    if (lo < AS_BASE) lo = AS_BASE;
    o->addr = lo;
    o->size = hi - lo;
    o->state = MS_FREE;
    o->perm = 0;
    o->attr = o->ipc = o->dev = 0;
    if (!held) memunlock();
}

static u64 find_free_nl(u64 lo, u64 hi, u64 size, u64 align) {
    if (!align) align = 0x1000;
    u64 a = (lo + align - 1) & ~(align - 1);
    for (;;) {
        if (a + size > hi || a + size < a) return 0;
        int clash = 0;
        for (int i = 0; i < nblk; i++) {
            MemBlock* b = &blk[i];
            if (b->addr < a + size && a < b->addr + b->size) {
                a = (b->addr + b->size + align - 1) & ~(align - 1);
                clash = 1;
                break;
            }
        }
        if (!clash) return a;
    }
}
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) {
    memlock();
    u64 a = find_free_nl(lo, hi, size, align);
    memunlock();
    return a;
}

/* finding and mapping in one hold, so two threads (CreateThread's TLS) can't be handed the same pages. The gap found
   has a page more on each side, left free as guard pages */
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    size = (size + 0xFFF) & ~0xFFFull;
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(0, 0, 1);
    memlock();
    u64 a = find_free_nl(KERN_BASE, KERN_END, size + 0x2000, 0x1000);
    int r = a ? map_nl(a + 0x1000, size, state, perm) : -1;
    memunlock();
    if (g_gpu_mapping_end) g_gpu_mapping_end();
    if (r) fatal("kmem_alloc: out of memory (%llx)", (unsigned long long)size);
    return a + 0x1000;
}

/* Budget checks, mapping and accounting share the physical-memory transaction's lock. In particular, a
   concurrent physical map cannot spend the same last pages as a heap extension. */
int mem_heap_set(u64 size, u64 budget) {
    if (size > HEAP_SIZE) return MEM_NOMEM;
    /* A repeated size has no backing change to protect. The atomic load is this no-op's linearization point. */
    if (size == __atomic_load_n(&g_heap_size, __ATOMIC_SEQ_CST)) return 0;
    if (g_gpu_mapping_begin) g_gpu_mapping_begin(0, 0, 1);
    memlock();
    u64 cur = g_heap_size;
    int r = 0;
    if (size > cur) {
        if (!budget_fits(size, g_phys_mapped, budget))
            r = MEM_LIMIT;
        else
            r = map_nl(HEAP_BASE + cur, size - cur, MS_NORMAL, PERM_RW);
    } else if (size < cur) {
        /* Locked, shared or otherwise changed heap pages cannot be discarded by shrinking the heap. */
        u64 lo = HEAP_BASE + size, end = HEAP_BASE + cur, covered = 0;
        for (int i = 0; i < nblk; i++) {
            MemBlock* b = &blk[i];
            if (b->addr >= end || b->addr + b->size <= lo) continue;
            if (b->state != MS_NORMAL || b->perm != PERM_RW || b->attr || b->ipc || b->dev) {
                r = MEM_BADSTATE;
                break;
            }
            u64 a = b->addr > lo ? b->addr : lo;
            u64 e = b->addr + b->size < end ? b->addr + b->size : end;
            covered += e - a;
        }
        if (!r && covered != cur - size) r = MEM_BADSTATE;
        if (!r && !reserve_blocks(2)) r = MEM_NOMEM;
        if (!r) unmap_nl(lo, cur - size);
    }
    if (!r) __atomic_store_n(&g_heap_size, size, __ATOMIC_SEQ_CST);
    memunlock();
    if (g_gpu_mapping_end) g_gpu_mapping_end();
    return r;
}
