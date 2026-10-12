/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_pool.h"
#include "gpu/nvn_alias.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <string.h>

/* Observe exported pool initialization after the guest SDK has completed it. Inlined builder setters are covered
   because the builder is read at Initialize, rather than mirrored call by call. This table owns no guest memory. */
typedef struct {
    u64 key, storage, size, mapped, gpu_address, generation;
    u32 flags;
    u8 alive, mapped_seen, gpu_seen;
} Pool;
#define POOL_SLOTS 8192
static Pool pools[POOL_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, live, reinitialized, unknown_finalizes, unknown_maps, unknown_addresses;
static u64 invalid_builders, table_full, mapped, addressed, inferred_addresses, getter_mismatches;

static Pool* pool_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = (u32)((key >> 4) * 2654435761u) & (POOL_SLOTS - 1);
    for (u32 step = 0; step < POOL_SLOTS; step++) {
        Pool* p = &pools[(home + step) & (POOL_SLOTS - 1)];
        if (p->key == key) return p;
        if (!p->key) return create ? p : NULL;
    }
    return NULL;
}

static int readable_range(u64 addr, u64 size) {
    if (addr < AS_BASE || addr >= AS_SIZE || size > AS_SIZE - addr) return 0;
    u64 end = addr + size;
    while (addr < end) {
        MemBlock block;
        mem_query(addr, &block);
        if (block.addr > addr || block.state == MS_FREE || block.state == MS_INACCESSIBLE || !(block.perm & PERM_R) ||
            block.size > AS_SIZE - block.addr || block.addr + block.size <= addr)
            return 0;
        u64 next = block.addr + block.size;
        addr = next < end ? next : end;
    }
    return 1;
}

int nvn_pool_wants(const char* name) {
    return !strcmp(name, "nvnMemoryPoolInitialize") || !strcmp(name, "nvnMemoryPoolFinalize") ||
           !strcmp(name, "nvnMemoryPoolMap") || !strcmp(name, "nvnMemoryPoolGetBufferAddress");
}

void nvn_pool_before(const CPU* c, const char* name, NvnPoolCall* call) {
    memset(call, 0, sizeof *call);
    call->pool = c->x[0];
    if (strcmp(name, "nvnMemoryPoolInitialize")) return;
    /* SDK builder: flags at +4, backing pointer at +16, size at +24. Do not require the backing to be CPU-readable:
       some pools are GPU-only or virtual. Only the small builder snapshot itself is dereferenced. */
    u64 builder = c->x[1];
    if (!readable_range(builder, 0x20)) return;
    call->flags = gr32(builder + 4);
    call->storage = gr64(builder + 0x10);
    call->size = gr64(builder + 0x18);
    call->valid = 1;
}

void nvn_pool_after(const CPU* c, const char* name, const NvnPoolCall* call) {
    pm_lock(&lock);
    if (!strcmp(name, "nvnMemoryPoolInitialize")) {
        if (!(c->x[0] & 1)) {
            failed++;
        } else if (!call->valid) {
            invalid_builders++;
        } else {
            Pool* p = pool_slot(call->pool, 1);
            if (!p) {
                table_full++;
            } else {
                if (p->alive) {
                    reinitialized++;
                } else {
                    live++;
                }
                p->key = call->pool;
                p->storage = call->storage;
                p->size = call->size;
                p->flags = call->flags;
                p->mapped = p->gpu_address = 0;
                p->mapped_seen = p->gpu_seen = 0;
                p->generation++;
                p->alive = 1;
                initialized++;
            }
        }
    } else {
        Pool* p = pool_slot(call->pool, 0);
        if (!strcmp(name, "nvnMemoryPoolFinalize")) {
            if (!p || !p->alive)
                unknown_finalizes++;
            else {
                nvn_alias_retire_pool(p->key, p->generation);
                p->alive = 0;
                live--;
                finalized++;
            }
        } else if (!strcmp(name, "nvnMemoryPoolMap")) {
            if (!p || !p->alive)
                unknown_maps++;
            else {
                p->mapped = c->x[0];
                p->mapped_seen = 1;
                mapped++;
            }
        } else if (!strcmp(name, "nvnMemoryPoolGetBufferAddress")) {
            if (!p || !p->alive)
                unknown_addresses++;
            else {
                if (p->gpu_seen && p->gpu_address != c->x[0]) getter_mismatches++;
                p->gpu_address = c->x[0];
                p->gpu_seen = 1;
                addressed++;
            }
        }
    }
    pm_unlock(&lock);
}

void nvn_pool_report(void) {
    pm_lock(&lock);
    LOG("nvn-pool: initialized=%llu failed=%llu finalized=%llu live=%llu reinitialized=%llu invalid-builders=%llu table-full=%llu\n",
        (unsigned long long)initialized, (unsigned long long)failed, (unsigned long long)finalized,
        (unsigned long long)live, (unsigned long long)reinitialized, (unsigned long long)invalid_builders,
        (unsigned long long)table_full);
    LOG("nvn-pool: maps=%llu gpu-addresses=%llu unknown-finalizes=%llu unknown-maps=%llu unknown-addresses=%llu\n",
        (unsigned long long)mapped, (unsigned long long)addressed, (unsigned long long)unknown_finalizes,
        (unsigned long long)unknown_maps, (unsigned long long)unknown_addresses);
    LOG("nvn-pool: inferred-gpu-bases=%llu getter-mismatches=%llu\n", (unsigned long long)inferred_addresses,
        (unsigned long long)getter_mismatches);
    pm_unlock(&lock);
}

int nvn_pool_snapshot(u64 pool, NvnPoolSnapshot* out) {
    pm_lock(&lock);
    Pool* p = pool_slot(pool, 0);
    int found = p && p->alive;
    if (found) {
        out->size = p->size;
        out->generation = p->generation;
        out->gpu_address = p->gpu_address;
        out->storage = p->storage;
        out->gpu_seen = p->gpu_seen;
    }
    pm_unlock(&lock);
    return found;
}

int nvn_pool_observe_buffer_address(u64 pool, u64 generation, u64 offset, u64 address) {
    pm_lock(&lock);
    Pool* p = pool_slot(pool, 0);
    int result = 0;
    if (p && p->alive && p->generation == generation && address && address >= offset) {
        u64 base = address - offset;
        if (!p->gpu_seen) {
            p->gpu_address = base;
            p->gpu_seen = 1;
            inferred_addresses++;
            result = 1;
        } else {
            result = p->gpu_address == base ? 2 : 3;
        }
    }
    pm_unlock(&lock);
    return result;
}
