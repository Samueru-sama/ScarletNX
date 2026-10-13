/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_descriptor.h"
#include "gpu/nvn_pool.h"
#include "gpu/nvn_texture.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <string.h>

/* Observe the texture-ID path from descriptor registration to a single BindTexture call. This does not decode
   BindTextures arrays or replace the guest SDK's descriptor memory. */
typedef struct {
    u64 key, generation;
    u64 memory_pool, memory_pool_generation;
    i64 memory_offset;
    u32 count;
    u8 alive;
} TexturePool;
typedef struct {
    u64 pool, pool_generation, texture, texture_generation, view;
    u64 memory_pool, memory_pool_generation, descriptor_memory_pool, descriptor_memory_generation, gpu_address;
    u32 index;
    u32 tic[8];
    NvnTextureViewSelection view_selection;
    u8 view_known, tic_valid;
} Descriptor;
typedef struct {
    u64 key, pool, pool_generation;
} CommandBufferPool;
#define TEXTURE_POOL_SLOTS 1024
#define DESCRIPTOR_SLOTS   65536
#define COMMAND_POOL_SLOTS 8192
static TexturePool pools[TEXTURE_POOL_SLOTS];
static Descriptor descriptors[DESCRIPTOR_SLOTS];
static CommandBufferPool commands[COMMAND_POOL_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, registered, binds, resolved, live_pools, sole_pool_key;
static u64 inferred_bind_pools, resolved_inferred;
static u64 unknown_pools, invalid_ids, untracked_textures, missing_bind_pools, missing_descriptors, stale_textures;
static u64 pool_table_full, descriptor_table_full, command_table_full;
static u64 view_snapshots, view_snapshot_failures;
static u64 tic_snapshots, tic_gpu_snapshots, tic_storage_snapshots, tic_missing_base, tic_address_mismatch;

static int texture_current(const Descriptor* d, NvnTextureSnapshot* out) {
    if (d->tic_valid && d->memory_pool && d->descriptor_memory_pool) {
        NvnPoolSnapshot backing = {0}, descriptor_memory = {0};
        if (!nvn_pool_snapshot(d->memory_pool, &backing) ||
            backing.generation != d->memory_pool_generation ||
            !nvn_pool_snapshot(d->descriptor_memory_pool, &descriptor_memory) ||
            descriptor_memory.generation != d->descriptor_memory_generation)
            return 0;
        if (out)
            *out = (NvnTextureSnapshot){.generation = d->texture_generation,
                                        .pool = d->memory_pool,
                                        .pool_generation = d->memory_pool_generation,
                                        .gpu_address = d->gpu_address,
                                        .address_known = 1};
        return 1;
    }
    NvnTextureSnapshot texture = {0};
    if (!nvn_texture_snapshot(d->texture, &texture) || texture.generation != d->texture_generation) return 0;
    if (!texture.pool) {
        if (out) *out = texture;
        return 1;
    }
    NvnPoolSnapshot pool = {0};
    if (!nvn_pool_snapshot(texture.pool, &pool) || pool.generation != texture.pool_generation) return 0;
    if (out) *out = texture;
    return 1;
}

static u32 hash(u64 key, u32 mask) { return (u32)((key >> 4) * 2654435761u) & mask; }

static TexturePool* pool_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = hash(key, TEXTURE_POOL_SLOTS - 1);
    for (u32 step = 0; step < TEXTURE_POOL_SLOTS; step++) {
        TexturePool* p = &pools[(home + step) & (TEXTURE_POOL_SLOTS - 1)];
        if (p->key == key) return p;
        if (!p->key) return create ? p : NULL;
    }
    return NULL;
}

static Descriptor* descriptor_slot(u64 pool, u32 index, int create) {
    if (!pool) return NULL;
    u32 home = hash(pool ^ ((u64)index << 32), DESCRIPTOR_SLOTS - 1);
    for (u32 step = 0; step < DESCRIPTOR_SLOTS; step++) {
        Descriptor* d = &descriptors[(home + step) & (DESCRIPTOR_SLOTS - 1)];
        if (d->pool == pool && d->index == index) return d;
        if (!d->pool) return create ? d : NULL;
    }
    return NULL;
}

static CommandBufferPool* command_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = hash(key, COMMAND_POOL_SLOTS - 1);
    for (u32 step = 0; step < COMMAND_POOL_SLOTS; step++) {
        CommandBufferPool* b = &commands[(home + step) & (COMMAND_POOL_SLOTS - 1)];
        if (b->key == key) return b;
        if (!b->key) return create ? b : NULL;
    }
    return NULL;
}

int nvn_descriptor_wants(const char* name) {
    return !strcmp(name, "nvnTexturePoolInitialize") || !strcmp(name, "nvnTexturePoolFinalize") ||
           !strcmp(name, "nvnTexturePoolRegisterTexture") || !strcmp(name, "nvnTexturePoolRegisterImage") ||
           !strcmp(name, "nvnCommandBufferSetTexturePool") ||
           !strcmp(name, "nvnCommandBufferBindTexture");
}

static int registering_descriptor(const char* name) {
    return !strcmp(name, "nvnTexturePoolRegisterTexture") || !strcmp(name, "nvnTexturePoolRegisterImage");
}

void nvn_descriptor_before(const CPU* c, const char* name, NvnDescriptorCall* call) {
    memset(call, 0, sizeof *call);
    call->object = c->x[0];
    if (!strcmp(name, "nvnTexturePoolInitialize")) {
        call->count = (u32)c->x[3];
        call->memory_pool = c->x[1];
        call->memory_offset = (i64)c->x[2];
    } else if (registering_descriptor(name)) {
        call->index = (u32)c->x[1];
        call->texture = c->x[2];
        call->view = c->x[3];
        call->view_known = !call->view || nvn_texture_view_snapshot(call->view, &call->view_selection);
    } else if (!strcmp(name, "nvnCommandBufferSetTexturePool")) {
        call->pool = c->x[1];
    } else if (!strcmp(name, "nvnCommandBufferBindTexture")) {
        call->handle = c->x[3];
        call->index = (u32)call->handle & 0xfffff; /* DeviceGetTextureHandle packs texture ID in low 20 bits. */
    }
}

void nvn_descriptor_after(const CPU* c, const char* name, const NvnDescriptorCall* call) {
    NvnTextureSnapshot texture = {0};
    int texture_found = 0;
    if (registering_descriptor(name)) texture_found = nvn_texture_snapshot(call->texture, &texture);
    NvnTextureTarget copied = {0};
    int copied_found = registering_descriptor(name) && !texture_found &&
                       nvn_texture_resolve_copied_target(call->texture, NULL, &copied) == NVN_TARGET_RESOLVED;
    NvnPoolSnapshot memory = {0};
    int memory_found =
        !strcmp(name, "nvnTexturePoolInitialize") && call->memory_pool && nvn_pool_snapshot(call->memory_pool, &memory);
    TexturePool registration_pool = {0};
    u32 tic[8] = {0};
    int tic_state = 0; /* 0 = no readable base, 1 = GPU copy, 2 = storage copy, 3 = address mismatch */
    if (registering_descriptor(name)) {
        pm_lock(&lock);
        TexturePool* p = pool_slot(call->object, 0);
        if (p && p->alive && call->index < p->count) registration_pool = *p;
        pm_unlock(&lock);
        u64 expected_address = texture_found ? texture.gpu_address : copied.gpu_address;
        if (registration_pool.alive && (texture_found ? texture.address_known : copied_found) &&
            registration_pool.memory_pool &&
            registration_pool.memory_offset >= 0) {
            NvnPoolSnapshot backing = {0};
            if (nvn_pool_snapshot(registration_pool.memory_pool, &backing) &&
                backing.generation == registration_pool.memory_pool_generation &&
                (u64)registration_pool.memory_offset <= backing.size &&
                (u64)registration_pool.count <= (backing.size - (u64)registration_pool.memory_offset) / 32) {
                u64 offset = (u64)registration_pool.memory_offset + (u64)call->index * 32;
                /* Registration writes the TIC before returning. Avoid a renderer readback on the guest CPU thread. */
                int copied = nvn_pool_read_backing(registration_pool.memory_pool,
                                                   registration_pool.memory_pool_generation, offset, tic, sizeof tic);
                if (copied) {
                    u64 tic_address = ((u64)(tic[2] & 0xffff) << 32) | tic[1];
                    tic_state = tic_address == expected_address ? copied : 3;
                }
            }
        }
    }
    pm_lock(&lock);
    if (!strcmp(name, "nvnTexturePoolInitialize")) {
        if (!c->x[0]) {
            failed++;
        } else {
            TexturePool* p = pool_slot(call->object, 1);
            if (!p)
                pool_table_full++;
            else {
                if (!p->alive) {
                    live_pools++;
                    sole_pool_key = live_pools == 1 ? call->object : 0;
                }
                p->key = call->object;
                p->generation++;
                p->count = call->count;
                p->memory_pool = call->memory_pool;
                p->memory_pool_generation = memory_found ? memory.generation : 0;
                p->memory_offset = call->memory_offset;
                p->alive = 1;
                initialized++;
            }
        }
    } else if (!strcmp(name, "nvnTexturePoolFinalize")) {
        TexturePool* p = pool_slot(call->object, 0);
        if (p && p->alive) {
            p->alive = 0;
            live_pools--;
            if (live_pools == 1) {
                for (u32 i = 0; i < TEXTURE_POOL_SLOTS; i++)
                    if (pools[i].alive) sole_pool_key = pools[i].key;
            } else {
                sole_pool_key = 0;
            }
            finalized++;
        } else {
            unknown_pools++;
        }
    } else if (registering_descriptor(name)) {
        TexturePool* p = pool_slot(call->object, 0);
        if (!p || !p->alive)
            unknown_pools++;
        else if (call->index >= p->count)
            invalid_ids++;
        else if (p->generation != registration_pool.generation)
            unknown_pools++;
        else {
            Descriptor* d = descriptor_slot(call->object, call->index, 1);
            if (!d)
                descriptor_table_full++;
            else {
                *d = (Descriptor){.pool = call->object,
                                  .pool_generation = p->generation,
                                  .texture = call->texture,
                                  .texture_generation = texture_found ? texture.generation
                                                        : copied_found ? copied.texture_generation : 0,
                                  .memory_pool = texture_found ? texture.pool : copied.pool,
                                  .memory_pool_generation = texture_found ? texture.pool_generation
                                                            : copied.pool_generation,
                                  .descriptor_memory_pool = registration_pool.memory_pool,
                                  .descriptor_memory_generation = registration_pool.memory_pool_generation,
                                  .gpu_address = texture_found ? texture.gpu_address : copied.gpu_address,
                                  .view = call->view,
                                  .index = call->index,
                                  .view_selection = call->view_selection,
                                  .view_known = call->view_known};
                if (tic_state == 1 || tic_state == 2) {
                    memcpy(d->tic, tic, sizeof tic);
                    d->tic_valid = 1;
                    tic_snapshots++;
                    if (tic_state == 1)
                        tic_gpu_snapshots++;
                    else
                        tic_storage_snapshots++;
                } else if (tic_state == 3) {
                    tic_address_mismatch++;
                } else {
                    tic_missing_base++;
                }
                registered++;
                if (!texture_found && !copied_found) untracked_textures++;
                if (call->view) {
                    if (call->view_known)
                        view_snapshots++;
                    else
                        view_snapshot_failures++;
                }
            }
        }
    } else if (!strcmp(name, "nvnCommandBufferSetTexturePool")) {
        TexturePool* p = pool_slot(call->pool, 0);
        CommandBufferPool* b = command_slot(call->object, 1);
        if (!b)
            command_table_full++;
        else {
            b->key = call->object;
            b->pool = call->pool;
            b->pool_generation = p && p->alive ? p->generation : 0;
            if (!p || !p->alive) unknown_pools++;
        }
    } else if (!strcmp(name, "nvnCommandBufferBindTexture")) {
        binds++;
        CommandBufferPool* b = command_slot(call->object, 0);
        TexturePool* p = b ? pool_slot(b->pool, 0) : NULL;
        int explicit_pool = b && p && p->alive && p->generation == b->pool_generation;
        if (!explicit_pool && live_pools == 1) {
            p = pool_slot(sole_pool_key, 0);
            inferred_bind_pools++;
        }
        if (!p || !p->alive || (!explicit_pool && live_pools != 1)) {
            missing_bind_pools++;
        } else {
            Descriptor* d = descriptor_slot(p->key, call->index, 0);
            if (!d || d->pool_generation != p->generation) {
                missing_descriptors++;
            } else {
                if (!texture_current(d, NULL))
                    stale_textures++;
                else {
                    resolved++;
                    if (!explicit_pool) resolved_inferred++;
                }
            }
        }
    }
    pm_unlock(&lock);
}

void nvn_descriptor_report(void) {
    pm_lock(&lock);
    LOG("nvn-descriptor: pools=%llu failed=%llu finalized=%llu registrations=%llu binds=%llu resolved=%llu inferred-bind-pools=%llu resolved-inferred=%llu\n",
        (unsigned long long)initialized, (unsigned long long)failed, (unsigned long long)finalized,
        (unsigned long long)registered, (unsigned long long)binds, (unsigned long long)resolved,
        (unsigned long long)inferred_bind_pools, (unsigned long long)resolved_inferred);
    LOG("nvn-descriptor: unknown-pools=%llu invalid-ids=%llu untracked-textures=%llu missing-bind-pools=%llu missing-descriptors=%llu stale-textures=%llu\n",
        (unsigned long long)unknown_pools, (unsigned long long)invalid_ids, (unsigned long long)untracked_textures,
        (unsigned long long)missing_bind_pools, (unsigned long long)missing_descriptors,
        (unsigned long long)stale_textures);
    LOG("nvn-descriptor: pool-table-full=%llu descriptor-table-full=%llu command-table-full=%llu\n",
        (unsigned long long)pool_table_full, (unsigned long long)descriptor_table_full,
        (unsigned long long)command_table_full);
    LOG("nvn-descriptor-view: copied=%llu failed=%llu\n", (unsigned long long)view_snapshots,
        (unsigned long long)view_snapshot_failures);
    LOG("nvn-descriptor-tic: copied=%llu gpu=%llu storage=%llu missing-base=%llu address-mismatch=%llu\n",
        (unsigned long long)tic_snapshots, (unsigned long long)tic_gpu_snapshots,
        (unsigned long long)tic_storage_snapshots, (unsigned long long)tic_missing_base,
        (unsigned long long)tic_address_mismatch);
    pm_unlock(&lock);
}

int nvn_descriptor_check(u64 pool, u64 handle) {
    pm_lock(&lock);
    TexturePool* p = pool_slot(pool, 0);
    Descriptor* d = p && p->alive ? descriptor_slot(pool, (u32)handle & 0xfffff, 0) : NULL;
    int result = 0;
    if (d && d->pool_generation == p->generation) { result = texture_current(d, NULL) ? 1 : -1; }
    pm_unlock(&lock);
    return result;
}

int nvn_descriptor_snapshot(u64 pool, u64 handle, NvnDescriptorSnapshot* out) {
    *out = (NvnDescriptorSnapshot){0};
    pm_lock(&lock);
    TexturePool* p = pool_slot(pool, 0);
    Descriptor* d = p && p->alive ? descriptor_slot(pool, (u32)handle & 0xfffff, 0) : NULL;
    int result = 0;
    if (d && d->pool_generation == p->generation) {
        NvnTextureSnapshot texture = {0};
        if (texture_current(d, &texture)) {
            *out = (NvnDescriptorSnapshot){.texture = d->texture,
                                           .view = d->view,
                                           .texture_generation = d->texture_generation,
                                           .pool_generation = d->pool_generation,
                                           .memory_pool = texture.pool,
                                           .memory_pool_generation = texture.pool_generation,
                                           .view_selection = d->view_selection,
                                           .view_known = d->view_known,
                                           .tic_valid = d->tic_valid};
            memcpy(out->tic, d->tic, sizeof d->tic);
            result = 1;
        } else {
            result = -1;
        }
    }
    pm_unlock(&lock);
    return result;
}
