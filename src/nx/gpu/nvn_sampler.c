/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_sampler.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <string.h>

/* Capture setter arguments: the inspected SDKs copy different builder layouts into sampler objects. */
typedef struct {
    u64 key, generation;
    NvnSamplerState state;
    u8 alive;
} Sampler;
typedef struct {
    u64 key;
    NvnSamplerState state;
} SamplerBuilder;
#define SAMPLER_SLOTS 16384
#define BUILDER_SLOTS 4096
static Sampler samplers[SAMPLER_SLOTS];
static SamplerBuilder builders[BUILDER_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, live, reinitialized, unknown_finalizes, table_full;
static u64 builder_defaults, builder_filters, builder_wraps, builder_table_full, state_filter_wrap;
static u64 builder_lod_clamp, builder_lod_bias, builder_compare, builder_anisotropy, builder_reduction,
    builder_lod_snap;
static u64 builder_border, builder_border_invalid;

static int readable_border(u64 addr) {
    if (addr < AS_BASE || addr >= AS_SIZE - 16) return 0;
    MemBlock block;
    mem_query(addr, &block);
    return block.addr <= addr && block.size >= 16 && addr - block.addr <= block.size - 16 && block.state != MS_FREE &&
           block.state != MS_INACCESSIBLE && (block.perm & PERM_R);
}

static SamplerBuilder* builder_slot(u64 key) {
    if (!key) return NULL;
    u32 home = (u32)((key >> 4) * 2654435761u) & (BUILDER_SLOTS - 1);
    for (u32 step = 0; step < BUILDER_SLOTS; step++) {
        SamplerBuilder* b = &builders[(home + step) & (BUILDER_SLOTS - 1)];
        if (b->key == key || !b->key) return b;
    }
    return NULL;
}

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
    return !strcmp(name, "nvnSamplerInitialize") || !strcmp(name, "nvnSamplerFinalize") ||
           !strcmp(name, "nvnSamplerBuilderSetDefaults") || !strcmp(name, "nvnSamplerBuilderSetMinMagFilter") ||
           !strcmp(name, "nvnSamplerBuilderSetWrapMode") || !strcmp(name, "nvnSamplerBuilderSetLodClamp") ||
           !strcmp(name, "nvnSamplerBuilderSetLodBias") || !strcmp(name, "nvnSamplerBuilderSetCompare") ||
           !strcmp(name, "nvnSamplerBuilderSetMaxAnisotropy") || !strcmp(name, "nvnSamplerBuilderSetReductionFilter") ||
           !strcmp(name, "nvnSamplerBuilderSetLodSnap") || !strcmp(name, "nvnSamplerBuilderSetBorderColor") ||
           !strcmp(name, "nvnSamplerBuilderSetBorderColori") || !strcmp(name, "nvnSamplerBuilderSetBorderColorui");
}

void nvn_sampler_before(const CPU* c, const char* name, NvnSamplerCall* call) {
    *call = (NvnSamplerCall){.sampler = c->x[0],
                             .builder = !strcmp(name, "nvnSamplerInitialize") ? c->x[1] : c->x[0],
                             .a = (u32)c->x[1],
                             .b = (u32)c->x[2],
                             .d = (u32)c->x[3],
                             .f0 = c->v[0].f[0],
                             .f1 = c->v[1].f[0]};
    if ((!strcmp(name, "nvnSamplerBuilderSetBorderColor") || !strcmp(name, "nvnSamplerBuilderSetBorderColori") ||
         !strcmp(name, "nvnSamplerBuilderSetBorderColorui")) &&
        readable_border(c->x[1])) {
        memcpy(call->border_color, GP(c->x[1]), sizeof call->border_color);
        call->border_valid = 1;
    }
}

void nvn_sampler_after(const CPU* c, const char* name, const NvnSamplerCall* call) {
    pm_lock(&lock);
    if (!strncmp(name, "nvnSamplerBuilderSet", 20)) {
        SamplerBuilder* b = builder_slot(call->builder);
        if (!b)
            builder_table_full++;
        else {
            b->key = call->builder;
            if (!strcmp(name, "nvnSamplerBuilderSetDefaults")) {
                b->state = (NvnSamplerState){0};
                builder_defaults++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetMinMagFilter")) {
                b->state.min_filter = call->a;
                b->state.mag_filter = call->b;
                b->state.known |= NVN_SAMPLER_FILTER_KNOWN;
                builder_filters++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetWrapMode")) {
                b->state.wrap_s = call->a;
                b->state.wrap_t = call->b;
                b->state.wrap_r = call->d;
                b->state.known |= NVN_SAMPLER_WRAP_KNOWN;
                builder_wraps++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetLodClamp")) {
                b->state.lod_min = call->f0;
                b->state.lod_max = call->f1;
                b->state.known |= NVN_SAMPLER_LOD_CLAMP_KNOWN;
                builder_lod_clamp++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetLodBias")) {
                b->state.lod_bias = call->f0;
                b->state.known |= NVN_SAMPLER_LOD_BIAS_KNOWN;
                builder_lod_bias++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetCompare")) {
                b->state.compare_mode = call->a;
                b->state.compare_func = call->b;
                b->state.known |= NVN_SAMPLER_COMPARE_KNOWN;
                builder_compare++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetMaxAnisotropy")) {
                b->state.anisotropy = call->f0;
                b->state.known |= NVN_SAMPLER_ANISOTROPY_KNOWN;
                builder_anisotropy++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetReductionFilter")) {
                b->state.reduction = call->a;
                b->state.known |= NVN_SAMPLER_REDUCTION_KNOWN;
                builder_reduction++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetLodSnap")) {
                b->state.lod_snap = call->f0;
                b->state.known |= NVN_SAMPLER_LOD_SNAP_KNOWN;
                builder_lod_snap++;
            } else if (!strcmp(name, "nvnSamplerBuilderSetBorderColor") ||
                       !strcmp(name, "nvnSamplerBuilderSetBorderColori") ||
                       !strcmp(name, "nvnSamplerBuilderSetBorderColorui")) {
                if (call->border_valid) {
                    memcpy(b->state.border_color, call->border_color, sizeof call->border_color);
                    b->state.border_type = !strcmp(name, "nvnSamplerBuilderSetBorderColor")    ? 0
                                           : !strcmp(name, "nvnSamplerBuilderSetBorderColori") ? 1
                                                                                               : 2;
                    b->state.known |= NVN_SAMPLER_BORDER_KNOWN;
                    builder_border++;
                } else {
                    b->state.known &= ~NVN_SAMPLER_BORDER_KNOWN;
                    builder_border_invalid++;
                }
            }
        }
    } else if (!strcmp(name, "nvnSamplerInitialize")) {
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
                SamplerBuilder* b = builder_slot(call->builder);
                s->state = b && b->key == call->builder ? b->state : (NvnSamplerState){0};
                if ((s->state.known & (NVN_SAMPLER_FILTER_KNOWN | NVN_SAMPLER_WRAP_KNOWN)) ==
                    (NVN_SAMPLER_FILTER_KNOWN | NVN_SAMPLER_WRAP_KNOWN))
                    state_filter_wrap++;
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
    if (found) {
        out->generation = s->generation;
        out->state = s->state;
    }
    pm_unlock(&lock);
    return found;
}

void nvn_sampler_report(void) {
    pm_lock(&lock);
    LOG("nvn-sampler: initialized=%llu failed=%llu finalized=%llu live=%llu reinitialized=%llu unknown-finalizes=%llu table-full=%llu\n",
        (unsigned long long)initialized, (unsigned long long)failed, (unsigned long long)finalized,
        (unsigned long long)live, (unsigned long long)reinitialized, (unsigned long long)unknown_finalizes,
        (unsigned long long)table_full);
    LOG("nvn-sampler-builder: defaults=%llu filter=%llu wrap=%llu table-full=%llu initialized-filter-wrap=%llu\n",
        (unsigned long long)builder_defaults, (unsigned long long)builder_filters, (unsigned long long)builder_wraps,
        (unsigned long long)builder_table_full, (unsigned long long)state_filter_wrap);
    LOG("nvn-sampler-builder: lod-clamp=%llu lod-bias=%llu compare=%llu anisotropy=%llu reduction=%llu lod-snap=%llu\n",
        (unsigned long long)builder_lod_clamp, (unsigned long long)builder_lod_bias,
        (unsigned long long)builder_compare, (unsigned long long)builder_anisotropy,
        (unsigned long long)builder_reduction, (unsigned long long)builder_lod_snap);
    LOG("nvn-sampler-builder: border=%llu invalid-border=%llu\n", (unsigned long long)builder_border,
        (unsigned long long)builder_border_invalid);
    pm_unlock(&lock);
}
