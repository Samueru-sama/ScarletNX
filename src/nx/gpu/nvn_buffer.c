/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_buffer.h"
#include "gpu/nvn_alias.h"
#include "gpu/nvn_pool.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <string.h>

/* Diagnostic buffer views. The guest SDK still creates and renders every buffer. */
typedef struct {
    u64 key, pool, offset, size, pool_generation, address, mapped;
    u8 alive, address_seen, mapped_seen;
} Buffer;
#define BUFFER_SLOTS 65536
static Buffer buffers[BUFFER_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, live, reinitialized, invalid_builders, table_full;
static u64 untracked_pools, invalid_ranges, stale_pools, unknown_finalizes, unknown_maps, unknown_addresses;
static u64 maps, addresses, address_inferences, address_checks, address_mismatches;

static Buffer* buffer_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = (u32)((key >> 4) * 2654435761u) & (BUFFER_SLOTS - 1);
    for (u32 step = 0; step < BUFFER_SLOTS; step++) {
        Buffer* b = &buffers[(home + step) & (BUFFER_SLOTS - 1)];
        if (b->key == key) return b;
        if (!b->key) return create ? b : NULL;
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

int nvn_buffer_wants(const char* name) {
    return !strcmp(name, "nvnBufferInitialize") || !strcmp(name, "nvnBufferFinalize") ||
           !strcmp(name, "nvnBufferMap") || !strcmp(name, "nvnBufferGetAddress");
}

void nvn_buffer_before(const CPU* c, const char* name, NvnBufferCall* call) {
    memset(call, 0, sizeof *call);
    call->buffer = c->x[0];
    if (strcmp(name, "nvnBufferInitialize")) return;
    /* Both inspected SDKs keep pool, offset, and size at builder +16, +24, and +32. Snapshot at Initialize so
       inlined builder setters are included. The pool storage itself is never dereferenced here. */
    u64 builder = c->x[1];
    if (!readable_range(builder, 0x28)) return;
    call->pool = gr64(builder + 0x10);
    call->offset = gr64(builder + 0x18);
    call->size = gr64(builder + 0x20);
    call->valid = 1;
}

void nvn_buffer_after(const CPU* c, const char* name, const NvnBufferCall* call) {
    NvnPoolSnapshot pool = {0};
    int pool_found = 0;
    if (!strcmp(name, "nvnBufferInitialize") && c->x[0] && call->valid)
        pool_found = nvn_pool_snapshot(call->pool, &pool);
    pm_lock(&lock);
    if (!strcmp(name, "nvnBufferInitialize")) {
        if (!c->x[0]) {
            failed++;
        } else if (!call->valid) {
            invalid_builders++;
        } else {
            Buffer* b = buffer_slot(call->buffer, 1);
            if (!b) {
                table_full++;
            } else {
                if (b->alive)
                    reinitialized++;
                else
                    live++;
                b->key = call->buffer;
                b->pool = call->pool;
                b->offset = call->offset;
                b->size = call->size;
                b->pool_generation = pool_found ? pool.generation : 0;
                b->address = b->mapped = 0;
                b->address_seen = b->mapped_seen = 0;
                b->alive = 1;
                initialized++;
                nvn_alias_retire(NVN_ALIAS_BUFFER, b->key);
                if (!pool_found)
                    untracked_pools++;
                else if (b->offset > pool.size || b->size > pool.size - b->offset)
                    invalid_ranges++;
                else
                    nvn_alias_register(NVN_ALIAS_BUFFER, b->key, b->pool, b->pool_generation, b->offset, b->size);
            }
        }
    } else {
        Buffer* b = buffer_slot(call->buffer, 0);
        if (!strcmp(name, "nvnBufferFinalize")) {
            if (!b || !b->alive)
                unknown_finalizes++;
            else {
                nvn_alias_retire(NVN_ALIAS_BUFFER, b->key);
                b->alive = 0;
                live--;
                finalized++;
            }
        } else if (!b || !b->alive) {
            if (!strcmp(name, "nvnBufferMap"))
                unknown_maps++;
            else
                unknown_addresses++;
        } else {
            NvnPoolSnapshot current = {0};
            int found = nvn_pool_snapshot(b->pool, &current);
            if (!found || current.generation != b->pool_generation) stale_pools++;
            if (!strcmp(name, "nvnBufferMap")) {
                b->mapped = c->x[0];
                b->mapped_seen = 1;
                maps++;
            } else {
                b->address = c->x[0];
                b->address_seen = 1;
                addresses++;
                if (found && current.generation == b->pool_generation) {
                    int result = nvn_pool_observe_buffer_address(b->pool, b->pool_generation, b->offset, b->address);
                    if (result == 1) address_inferences++;
                    if (result >= 2) address_checks++;
                    if (result == 3) address_mismatches++;
                }
            }
        }
    }
    pm_unlock(&lock);
}

void nvn_buffer_report(void) {
    pm_lock(&lock);
    LOG("nvn-buffer: initialized=%llu failed=%llu finalized=%llu live=%llu reinitialized=%llu invalid-builders=%llu table-full=%llu\n",
        (unsigned long long)initialized, (unsigned long long)failed, (unsigned long long)finalized,
        (unsigned long long)live, (unsigned long long)reinitialized, (unsigned long long)invalid_builders,
        (unsigned long long)table_full);
    LOG("nvn-buffer: untracked-pools=%llu invalid-ranges=%llu stale-pools=%llu unknown-finalizes=%llu unknown-maps=%llu unknown-addresses=%llu\n",
        (unsigned long long)untracked_pools, (unsigned long long)invalid_ranges, (unsigned long long)stale_pools,
        (unsigned long long)unknown_finalizes, (unsigned long long)unknown_maps, (unsigned long long)unknown_addresses);
    LOG("nvn-buffer: maps=%llu addresses=%llu address-inferences=%llu address-checks=%llu address-mismatches=%llu\n",
        (unsigned long long)maps, (unsigned long long)addresses, (unsigned long long)address_inferences,
        (unsigned long long)address_checks, (unsigned long long)address_mismatches);
    pm_unlock(&lock);
}
