/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_sampler_descriptor.h"
#include "gpu/nvn_pool.h"
#include "gpu/nvn_sampler.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <string.h>

/* Pool-relative sampler IDs occupy bits 20..31 of a device texture handle. */
typedef struct {
    u64 key, generation;
    u64 memory_pool, memory_pool_generation;
    i64 memory_offset;
    u32 count;
    u8 alive;
} SamplerPool;
typedef struct {
    u64 pool, pool_generation, sampler, sampler_generation;
    u32 index;
    u32 tsc[8];
    u8 tsc_valid;
} SamplerDescriptor;
#define POOL_SLOTS       1024
#define DESCRIPTOR_SLOTS 32768
static SamplerPool pools[POOL_SLOTS];
static SamplerDescriptor descriptors[DESCRIPTOR_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, registered, live_pools;
static u64 unknown_pools, invalid_ids, untracked_samplers, pool_table_full, descriptor_table_full;
static u64 tsc_copied, tsc_storage, tsc_gpu, tsc_missing, tsc_empty;

static u32 hash(u64 key, u32 mask) { return (u32)((key >> 4) * 2654435761u) & mask; }

static SamplerPool* pool_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = hash(key, POOL_SLOTS - 1);
    for (u32 step = 0; step < POOL_SLOTS; step++) {
        SamplerPool* p = &pools[(home + step) & (POOL_SLOTS - 1)];
        if (p->key == key) return p;
        if (!p->key) return create ? p : NULL;
    }
    return NULL;
}

static SamplerDescriptor* descriptor_slot(u64 pool, u32 index, int create) {
    if (!pool) return NULL;
    u32 home = hash(pool ^ ((u64)index << 32), DESCRIPTOR_SLOTS - 1);
    for (u32 step = 0; step < DESCRIPTOR_SLOTS; step++) {
        SamplerDescriptor* d = &descriptors[(home + step) & (DESCRIPTOR_SLOTS - 1)];
        if (d->pool == pool && d->index == index) return d;
        if (!d->pool) return create ? d : NULL;
    }
    return NULL;
}

int nvn_sampler_descriptor_wants(const char* name) {
    return !strcmp(name, "nvnSamplerPoolInitialize") || !strcmp(name, "nvnSamplerPoolFinalize") ||
           !strcmp(name, "nvnSamplerPoolRegisterSampler");
}

void nvn_sampler_descriptor_before(const CPU* c, const char* name, NvnSamplerDescriptorCall* call) {
    memset(call, 0, sizeof *call);
    call->object = c->x[0];
    if (!strcmp(name, "nvnSamplerPoolInitialize")) {
        call->count = (u32)c->x[3];
        call->memory_pool = c->x[1];
        call->memory_offset = (i64)c->x[2];
    } else if (!strcmp(name, "nvnSamplerPoolRegisterSampler")) {
        call->index = (u32)c->x[1];
        call->sampler = c->x[2];
    }
}

void nvn_sampler_descriptor_after(const CPU* c, const char* name, const NvnSamplerDescriptorCall* call) {
    NvnSamplerSnapshot snapshot = {0};
    int found = !strcmp(name, "nvnSamplerPoolRegisterSampler") && nvn_sampler_snapshot(call->sampler, &snapshot);
    NvnPoolSnapshot memory = {0};
    int memory_found =
        !strcmp(name, "nvnSamplerPoolInitialize") && call->memory_pool && nvn_pool_snapshot(call->memory_pool, &memory);
    SamplerPool registration_pool = {0};
    u32 tsc[8] = {0};
    int tsc_source = 0, tsc_nonzero = 0;
    if (!strcmp(name, "nvnSamplerPoolRegisterSampler")) {
        pm_lock(&lock);
        SamplerPool* p = pool_slot(call->object, 0);
        if (p && p->alive && call->index < p->count) registration_pool = *p;
        pm_unlock(&lock);
        /* Some SDK clients initialize a temporary sampler and copy its object before registration. The TSC in
           descriptor memory is authoritative even when that copied object has no observed Initialize call. */
        if (registration_pool.alive && registration_pool.memory_pool && registration_pool.memory_offset >= 0) {
            NvnPoolSnapshot backing = {0};
            if (nvn_pool_snapshot(registration_pool.memory_pool, &backing) &&
                backing.generation == registration_pool.memory_pool_generation &&
                (u64)registration_pool.memory_offset <= backing.size &&
                (u64)registration_pool.count <= (backing.size - (u64)registration_pool.memory_offset) / 32) {
                u64 offset = (u64)registration_pool.memory_offset + (u64)call->index * 32;
                tsc_source = nvn_pool_read_backing(registration_pool.memory_pool,
                                                   registration_pool.memory_pool_generation, offset, tsc, sizeof tsc);
                for (u32 i = 0; i < 8; i++) tsc_nonzero |= tsc[i] != 0;
            }
        }
    }
    pm_lock(&lock);
    if (!strcmp(name, "nvnSamplerPoolInitialize")) {
        if (!c->x[0]) {
            failed++;
        } else {
            SamplerPool* p = pool_slot(call->object, 1);
            if (!p)
                pool_table_full++;
            else {
                if (!p->alive) live_pools++;
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
    } else if (!strcmp(name, "nvnSamplerPoolFinalize")) {
        SamplerPool* p = pool_slot(call->object, 0);
        if (p && p->alive) {
            p->alive = 0;
            live_pools--;
            finalized++;
        } else {
            unknown_pools++;
        }
    } else {
        SamplerPool* p = pool_slot(call->object, 0);
        if (!p || !p->alive)
            unknown_pools++;
        else if (call->index >= p->count)
            invalid_ids++;
        else if (p->generation != registration_pool.generation)
            unknown_pools++;
        else {
            SamplerDescriptor* d = descriptor_slot(call->object, call->index, 1);
            if (!d)
                descriptor_table_full++;
            else {
                *d = (SamplerDescriptor){.pool = call->object,
                                         .pool_generation = p->generation,
                                         .sampler = call->sampler,
                                         .sampler_generation = found ? snapshot.generation : 0,
                                         .index = call->index};
                if (tsc_source && tsc_nonzero) {
                    memcpy(d->tsc, tsc, sizeof tsc);
                    d->tsc_valid = 1;
                    tsc_copied++;
                    if (tsc_source == 2)
                        tsc_storage++;
                    else
                        tsc_gpu++;
                } else if (tsc_source) {
                    tsc_empty++;
                } else {
                    tsc_missing++;
                }
                registered++;
                if (!found) untracked_samplers++;
            }
        }
    }
    pm_unlock(&lock);
}

int nvn_sampler_descriptor_check(u64 pool, u64 handle) {
    pm_lock(&lock);
    SamplerPool* p = pool_slot(pool, 0);
    SamplerDescriptor* d = p && p->alive ? descriptor_slot(pool, ((u32)handle >> 20) & 0xfff, 0) : NULL;
    int result = 0;
    if (d && d->pool_generation == p->generation) {
        NvnSamplerSnapshot snapshot = {0};
        result = (!d->sampler_generation && d->tsc_valid) ||
                         (nvn_sampler_snapshot(d->sampler, &snapshot) && snapshot.generation == d->sampler_generation)
                     ? 1
                     : -1;
    }
    pm_unlock(&lock);
    return result;
}

int nvn_sampler_descriptor_snapshot(u64 pool, u64 handle, NvnSamplerDescriptorSnapshot* out) {
    *out = (NvnSamplerDescriptorSnapshot){0};
    pm_lock(&lock);
    SamplerPool* p = pool_slot(pool, 0);
    SamplerDescriptor* d = p && p->alive ? descriptor_slot(pool, ((u32)handle >> 20) & 0xfff, 0) : NULL;
    int result = 0;
    if (d && d->pool_generation == p->generation) {
        NvnSamplerSnapshot snapshot = {0};
        if ((!d->sampler_generation && d->tsc_valid) ||
            (nvn_sampler_snapshot(d->sampler, &snapshot) && snapshot.generation == d->sampler_generation)) {
            *out = (NvnSamplerDescriptorSnapshot){.sampler = d->sampler,
                                                  .sampler_generation = d->sampler_generation,
                                                  .pool_generation = d->pool_generation,
                                                  .state = snapshot.state,
                                                  .tsc_valid = d->tsc_valid};
            memcpy(out->tsc, d->tsc, sizeof d->tsc);
            result = 1;
        } else {
            result = -1;
        }
    }
    pm_unlock(&lock);
    return result;
}

void nvn_sampler_descriptor_report(void) {
    pm_lock(&lock);
    LOG("nvn-sampler-descriptor: pools=%llu failed=%llu finalized=%llu live=%llu registrations=%llu unknown-pools=%llu invalid-ids=%llu untracked-samplers=%llu pool-table-full=%llu descriptor-table-full=%llu\n",
        (unsigned long long)initialized, (unsigned long long)failed, (unsigned long long)finalized,
        (unsigned long long)live_pools, (unsigned long long)registered, (unsigned long long)unknown_pools,
        (unsigned long long)invalid_ids, (unsigned long long)untracked_samplers, (unsigned long long)pool_table_full,
        (unsigned long long)descriptor_table_full);
    LOG("nvn-sampler-tsc: copied=%llu storage=%llu gpu=%llu missing=%llu empty=%llu\n", (unsigned long long)tsc_copied,
        (unsigned long long)tsc_storage, (unsigned long long)tsc_gpu, (unsigned long long)tsc_missing,
        (unsigned long long)tsc_empty);
    pm_unlock(&lock);
}
