/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_sampler.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <string.h>

/* Identity and generation only. The inspected SDKs copy different builder layouts into sampler objects. */
typedef struct {
    u64 key, generation;
    u8 alive;
} Sampler;
#define SAMPLER_SLOTS 16384
static Sampler samplers[SAMPLER_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, live, reinitialized, unknown_finalizes, table_full;

static Sampler* sampler_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = (u32)((key >> 4) * 2654435761u) & (SAMPLER_SLOTS - 1);
    for (u32 step = 0; step < SAMPLER_SLOTS; step++) {
        Sampler* s = &samplers[(home + step) & (SAMPLER_SLOTS - 1)];
        if (s->key == key) return s;
        if (!s->key) return create ? s : NULL;
    }
    return NULL;
}

int nvn_sampler_wants(const char* name) {
    return !strcmp(name, "nvnSamplerInitialize") || !strcmp(name, "nvnSamplerFinalize");
}

void nvn_sampler_before(const CPU* c, NvnSamplerCall* call) { call->sampler = c->x[0]; }

void nvn_sampler_after(const CPU* c, const char* name, const NvnSamplerCall* call) {
    pm_lock(&lock);
    if (!strcmp(name, "nvnSamplerInitialize")) {
        if (!c->x[0]) {
            failed++;
        } else {
            Sampler* s = sampler_slot(call->sampler, 1);
            if (!s)
                table_full++;
            else {
                if (s->alive)
                    reinitialized++;
                else
                    live++;
                s->key = call->sampler;
                s->generation++;
                s->alive = 1;
                initialized++;
            }
        }
    } else {
        Sampler* s = sampler_slot(call->sampler, 0);
        if (!s || !s->alive)
            unknown_finalizes++;
        else {
            s->alive = 0;
            live--;
            finalized++;
        }
    }
    pm_unlock(&lock);
}

int nvn_sampler_snapshot(u64 sampler, NvnSamplerSnapshot* out) {
    pm_lock(&lock);
    Sampler* s = sampler_slot(sampler, 0);
    int found = s && s->alive;
    if (found) out->generation = s->generation;
    pm_unlock(&lock);
    return found;
}

void nvn_sampler_report(void) {
    pm_lock(&lock);
    LOG("nvn-sampler: initialized=%llu failed=%llu finalized=%llu live=%llu reinitialized=%llu unknown-finalizes=%llu table-full=%llu\n",
        (unsigned long long)initialized, (unsigned long long)failed, (unsigned long long)finalized,
        (unsigned long long)live, (unsigned long long)reinitialized, (unsigned long long)unknown_finalizes,
        (unsigned long long)table_full);
    pm_unlock(&lock);
}
