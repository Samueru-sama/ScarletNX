/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_texture.h"
#include "gpu/nvn_alias.h"
#include "gpu/nvn_pool.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <string.h>

/* Texture metadata and pool generations only. Storage layout and contents remain owned by the guest SDK. */
typedef struct {
    u64 key, generation, pool, offset, pool_generation, gpu_address;
    u32 flags, target, width, height, depth, levels, format, samples, storage_size;
    u8 alive, size_known, address_known;
} Texture;
#define TEXTURE_SLOTS 32768
static Texture textures[TEXTURE_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, live, reinitialized, invalid_builders, table_full;
static u64 no_pool, untracked_pools, offset_outside_pool, unknown_finalizes;
static u64 sizes_known, sizes_unavailable, footprint_outside_pool;
static u64 addresses_known, addresses_unavailable, address_mismatches;

static Texture* texture_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = (u32)((key >> 4) * 2654435761u) & (TEXTURE_SLOTS - 1);
    for (u32 step = 0; step < TEXTURE_SLOTS; step++) {
        Texture* t = &textures[(home + step) & (TEXTURE_SLOTS - 1)];
        if (t->key == key) return t;
        if (!t->key) return create ? t : NULL;
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

int nvn_texture_wants(const char* name) {
    return !strcmp(name, "nvnTextureInitialize") || !strcmp(name, "nvnTextureFinalize");
}

void nvn_texture_before(const CPU* c, const char* name, NvnTextureCall* call) {
    memset(call, 0, sizeof *call);
    call->texture = c->x[0];
    if (strcmp(name, "nvnTextureInitialize")) return;
    /* These builder fields agree in both inspected SDKs. Do not derive storage size from dimensions: compressed,
       tiled, multisample, and mipmapped textures need the SDK's layout calculation. */
    u64 builder = c->x[1];
    if (!readable_range(builder, 0x40)) return;
    call->pool = gr64(builder + 0x10);
    call->offset = gr64(builder + 0x18);
    call->flags = gr32(builder + 0x20);
    call->target = gr32(builder + 0x24);
    call->width = gr32(builder + 0x28);
    call->height = gr32(builder + 0x2c);
    call->depth = gr32(builder + 0x30);
    call->levels = gr32(builder + 0x34);
    call->format = gr32(builder + 0x38);
    call->samples = gr32(builder + 0x3c);
    call->valid = 1;
}

void nvn_texture_after(const CPU* c, const char* name, const NvnTextureCall* call) {
    NvnPoolSnapshot pool = {0};
    int pool_found = 0;
    if (!strcmp(name, "nvnTextureInitialize") && c->x[0] && call->valid && call->pool)
        pool_found = nvn_pool_snapshot(call->pool, &pool);
    pm_lock(&lock);
    if (!strcmp(name, "nvnTextureInitialize")) {
        if (!c->x[0]) {
            failed++;
        } else if (!call->valid) {
            invalid_builders++;
        } else {
            Texture* t = texture_slot(call->texture, 1);
            if (!t) {
                table_full++;
            } else {
                if (t->alive)
                    reinitialized++;
                else
                    live++;
                t->key = call->texture;
                t->generation++;
                t->pool = call->pool;
                t->offset = call->offset;
                t->pool_generation = pool_found ? pool.generation : 0;
                t->flags = call->flags;
                t->target = call->target;
                t->width = call->width;
                t->height = call->height;
                t->depth = call->depth;
                t->levels = call->levels;
                t->format = call->format;
                t->samples = call->samples;
                t->storage_size = call->storage_size;
                t->size_known = call->size_known;
                t->gpu_address = call->gpu_address;
                t->address_known = call->address_known;
                t->alive = 1;
                initialized++;
                nvn_alias_retire(NVN_ALIAS_TEXTURE, t->key);
                if (call->size_known)
                    sizes_known++;
                else
                    sizes_unavailable++;
                if (call->address_known) {
                    addresses_known++;
                    if (pool_found && pool.gpu_seen && call->offset < pool.size &&
                        (pool.gpu_address > UINT64_MAX - call->offset ||
                         pool.gpu_address + call->offset != call->gpu_address))
                        address_mismatches++;
                } else {
                    addresses_unavailable++;
                }
                if (!call->pool)
                    no_pool++;
                else if (!pool_found)
                    untracked_pools++;
                else if (call->offset >= pool.size)
                    offset_outside_pool++;
                else if (call->size_known && call->storage_size > pool.size - call->offset)
                    footprint_outside_pool++;
                else if (call->size_known)
                    nvn_alias_register(NVN_ALIAS_TEXTURE, t->key, t->pool, t->pool_generation, t->offset,
                                       t->storage_size);
            }
        }
    } else {
        Texture* t = texture_slot(call->texture, 0);
        if (!t || !t->alive)
            unknown_finalizes++;
        else {
            nvn_alias_retire(NVN_ALIAS_TEXTURE, t->key);
            t->alive = 0;
            live--;
            finalized++;
        }
    }
    pm_unlock(&lock);
}

void nvn_texture_report(void) {
    pm_lock(&lock);
    LOG("nvn-texture: initialized=%llu failed=%llu finalized=%llu live=%llu reinitialized=%llu invalid-builders=%llu table-full=%llu\n",
        (unsigned long long)initialized, (unsigned long long)failed, (unsigned long long)finalized,
        (unsigned long long)live, (unsigned long long)reinitialized, (unsigned long long)invalid_builders,
        (unsigned long long)table_full);
    LOG("nvn-texture: no-pool=%llu untracked-pools=%llu offset-outside-pool=%llu unknown-finalizes=%llu\n",
        (unsigned long long)no_pool, (unsigned long long)untracked_pools, (unsigned long long)offset_outside_pool,
        (unsigned long long)unknown_finalizes);
    LOG("nvn-texture: sizes-known=%llu sizes-unavailable=%llu footprint-outside-pool=%llu\n",
        (unsigned long long)sizes_known, (unsigned long long)sizes_unavailable,
        (unsigned long long)footprint_outside_pool);
    LOG("nvn-texture: addresses-known=%llu unavailable=%llu pool-mismatches=%llu\n",
        (unsigned long long)addresses_known, (unsigned long long)addresses_unavailable,
        (unsigned long long)address_mismatches);
    pm_unlock(&lock);
}

int nvn_texture_snapshot(u64 texture, NvnTextureSnapshot* out) {
    pm_lock(&lock);
    Texture* t = texture_slot(texture, 0);
    int found = t && t->alive;
    if (found) {
        out->generation = t->generation;
        out->pool = t->pool;
        out->pool_generation = t->pool_generation;
        out->offset = t->offset;
        out->storage_size = t->storage_size;
        out->gpu_address = t->gpu_address;
        out->address_known = t->address_known;
        out->flags = t->flags;
        out->target = t->target;
        out->width = t->width;
        out->height = t->height;
        out->depth = t->depth;
        out->levels = t->levels;
        out->format = t->format;
        out->samples = t->samples;
    }
    pm_unlock(&lock);
    return found;
}

NvnTargetResult nvn_texture_resolve_target(u64 texture, const NvnTextureViewSelection* view, NvnTextureTarget* out) {
    NvnTextureSnapshot t;
    if (!nvn_texture_snapshot(texture, &t)) return NVN_TARGET_MISSING;
    NvnPoolSnapshot pool;
    if (!nvn_pool_snapshot(t.pool, &pool) || pool.generation != t.pool_generation || t.offset >= pool.size ||
        t.storage_size > pool.size - t.offset)
        return NVN_TARGET_STALE_POOL;
    if (!t.address_known || !t.gpu_address) return NVN_TARGET_NO_ADDRESS;
    NvnTextureViewSelection selection = view ? *view : (NvnTextureViewSelection){0};
    if (selection.flags & ~0x3fu) return NVN_TARGET_BAD_VIEW;
    /* TotK depth views use a zero count. Preserve that SDK sentinel for the renderer to interpret. */
    if ((selection.flags & 1) && (selection.base_level >= t.levels ||
                                  (selection.level_count && selection.level_count > t.levels - selection.base_level)))
        return NVN_TARGET_BAD_VIEW;
    if ((selection.flags & 2) && (selection.first_layer >= t.depth ||
                                  (selection.layer_count && selection.layer_count > t.depth - selection.first_layer)))
        return NVN_TARGET_BAD_VIEW;
    *out = (NvnTextureTarget){.gpu_address = t.gpu_address,
                              .texture_generation = t.generation,
                              .pool_generation = t.pool_generation,
                              .width = t.width,
                              .height = t.height,
                              .depth = t.depth,
                              .levels = t.levels,
                              .format = (selection.flags & 4) ? selection.format : t.format,
                              .target = (selection.flags & 32) ? selection.target : t.target,
                              .samples = t.samples,
                              .flags = t.flags,
                              .view = selection};
    return NVN_TARGET_RESOLVED;
}
