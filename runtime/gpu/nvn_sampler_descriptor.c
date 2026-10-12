/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_sampler_descriptor.h"
#include "gpu/nvn_sampler.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <string.h>

/* Pool-relative sampler IDs occupy bits 20..31 of a device texture handle. */
typedef struct {
    u64 key, generation;
    u32 count;
    u8 alive;
} SamplerPool;
typedef struct {
    u64 pool, pool_generation, sampler, sampler_generation;
    u32 index;
} SamplerDescriptor;
#define POOL_SLOTS       1024
#define DESCRIPTOR_SLOTS 32768
static SamplerPool pools[POOL_SLOTS];
static SamplerDescriptor descriptors[DESCRIPTOR_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, registered, live_pools;
static u64 unknown_pools, invalid_ids, untracked_samplers, pool_table_full, descriptor_table_full;

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
    } else if (!strcmp(name, "nvnSamplerPoolRegisterSampler")) {
        call->index = (u32)c->x[1];
        call->sampler = c->x[2];
    }
}

void nvn_sampler_descriptor_after(const CPU* c, const char* name, const NvnSamplerDescriptorCall* call) {
    NvnSamplerSnapshot snapshot = {0};
    int found = !strcmp(name, "nvnSamplerPoolRegisterSampler") && nvn_sampler_snapshot(call->sampler, &snapshot);
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
        result = nvn_sampler_snapshot(d->sampler, &snapshot) && snapshot.generation == d->sampler_generation ? 1 : -1;
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
        if (nvn_sampler_snapshot(d->sampler, &snapshot) && snapshot.generation == d->sampler_generation) {
            *out = (NvnSamplerDescriptorSnapshot){.sampler = d->sampler,
                                                  .sampler_generation = d->sampler_generation,
                                                  .pool_generation = d->pool_generation};
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
    pm_unlock(&lock);
}
