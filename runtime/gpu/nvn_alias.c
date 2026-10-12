/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_alias.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* Pool-relative interval registry. A pool generation owns the list; views keep guest object identity separately.
   No guest storage is copied or mapped here. The registry is bounded and diagnostic until a renderer consumes it. */
typedef struct {
    u64 pool, generation, offset, size;
    u32 next;
    u8 kind, alive;
} View;
typedef struct {
    u64 key, generation;
    u32 head;
} PoolList;
typedef struct {
    u64 key;
    u32 index;
    u8 kind;
} Object;
#define ALIAS_POOL_SLOTS   8192
#define ALIAS_OBJECT_SLOTS 131072
#define ALIAS_VIEW_LIMIT   262144
static PoolList pool_lists[ALIAS_POOL_SLOTS];
static Object objects[ALIAS_OBJECT_SLOTS];
static View* views;
static u32 view_count, view_capacity;
static PMutex lock = PMUTEX_INIT;
static int enabled;
static u64 registered, retired, pool_retired, live, zero_sizes, invalid_ranges, pool_table_full, object_table_full;
static u64 view_table_full;
static u64 buffer_pairs, mixed_pairs, texture_pairs, exact_pairs;

static u32 hash(u64 key, u32 mask) { return (u32)((key >> 4) * 2654435761u) & mask; }

static PoolList* pool_slot(u64 key) {
    u32 home = hash(key, ALIAS_POOL_SLOTS - 1);
    for (u32 step = 0; step < ALIAS_POOL_SLOTS; step++) {
        PoolList* p = &pool_lists[(home + step) & (ALIAS_POOL_SLOTS - 1)];
        if (p->key == key || !p->key) return p;
    }
    return NULL;
}

static Object* object_slot(int kind, u64 key) {
    u32 home = hash(key ^ ((u64)kind << 48), ALIAS_OBJECT_SLOTS - 1);
    for (u32 step = 0; step < ALIAS_OBJECT_SLOTS; step++) {
        Object* o = &objects[(home + step) & (ALIAS_OBJECT_SLOTS - 1)];
        if ((o->key == key && o->kind == kind) || !o->key) return o;
    }
    return NULL;
}

void nvn_alias_enable(void) { enabled = 1; }

void nvn_alias_register(int kind, u64 object, u64 pool, u64 generation, u64 offset, u64 size) {
    if (!enabled) return;
    pm_lock(&lock);
    if (!object || !pool || !generation || (kind != NVN_ALIAS_BUFFER && kind != NVN_ALIAS_TEXTURE) ||
        size > UINT64_MAX - offset) {
        invalid_ranges++;
        goto done;
    }
    if (!size) {
        zero_sizes++;
        goto done;
    }
    PoolList* p = pool_slot(pool);
    Object* o = object_slot(kind, object);
    if (!p || !o) {
        if (!p) pool_table_full++;
        if (!o) object_table_full++;
        goto done;
    }
    if (view_count == ALIAS_VIEW_LIMIT) {
        view_table_full++;
        goto done;
    }
    if (view_count == view_capacity) {
        u32 capacity = view_capacity ? view_capacity * 2 : 1024;
        View* grown = realloc(views, (size_t)capacity * sizeof *grown);
        if (!grown) {
            view_table_full++;
            goto done;
        }
        views = grown;
        view_capacity = capacity;
    }
    if (o->key && o->index && views[o->index - 1].alive) {
        views[o->index - 1].alive = 0;
        live--;
    }
    if (!p->key) p->key = pool;
    if (p->generation != generation) {
        for (u32 index = p->head; index; index = views[index - 1].next) {
            View* old = &views[index - 1];
            if (old->alive) {
                old->alive = 0;
                live--;
                pool_retired++;
            }
        }
        p->generation = generation;
        p->head = 0;
    }
    u64 end = offset + size;
    for (u32 index = p->head; index; index = views[index - 1].next) {
        const View* other = &views[index - 1];
        if (!other->alive || offset >= other->offset + other->size || other->offset >= end) continue;
        if (kind == NVN_ALIAS_BUFFER && other->kind == NVN_ALIAS_BUFFER)
            buffer_pairs++;
        else if (kind == NVN_ALIAS_TEXTURE && other->kind == NVN_ALIAS_TEXTURE)
            texture_pairs++;
        else
            mixed_pairs++;
        if (offset == other->offset && size == other->size) exact_pairs++;
    }
    View* v = &views[view_count];
    *v = (View){.pool = pool,
                .generation = generation,
                .offset = offset,
                .size = size,
                .next = p->head,
                .kind = (u8)kind,
                .alive = 1};
    p->head = ++view_count;
    o->key = object;
    o->kind = (u8)kind;
    o->index = view_count;
    registered++;
    live++;
done:
    pm_unlock(&lock);
}

void nvn_alias_retire(int kind, u64 object) {
    if (!enabled || !object) return;
    pm_lock(&lock);
    Object* o = object_slot(kind, object);
    if (o && o->key == object && o->kind == kind && o->index && views[o->index - 1].alive) {
        views[o->index - 1].alive = 0;
        live--;
        retired++;
    }
    pm_unlock(&lock);
}

void nvn_alias_retire_pool(u64 pool, u64 generation) {
    if (!enabled || !pool || !generation) return;
    pm_lock(&lock);
    PoolList* p = pool_slot(pool);
    if (p && p->key == pool && p->generation == generation) {
        for (u32 index = p->head; index; index = views[index - 1].next) {
            View* v = &views[index - 1];
            if (v->alive) {
                v->alive = 0;
                live--;
                pool_retired++;
            }
        }
        p->head = 0;
    }
    pm_unlock(&lock);
}

void nvn_alias_report(void) {
    pm_lock(&lock);
    LOG("nvn-alias: registered=%llu retired=%llu pool-retired=%llu live=%llu buffer-pairs=%llu mixed-pairs=%llu texture-pairs=%llu exact-pairs=%llu\n",
        (unsigned long long)registered, (unsigned long long)retired, (unsigned long long)pool_retired,
        (unsigned long long)live, (unsigned long long)buffer_pairs, (unsigned long long)mixed_pairs,
        (unsigned long long)texture_pairs, (unsigned long long)exact_pairs);
    LOG("nvn-alias: zero-sizes=%llu invalid-ranges=%llu pool-table-full=%llu object-table-full=%llu view-table-full=%llu\n",
        (unsigned long long)zero_sizes, (unsigned long long)invalid_ranges, (unsigned long long)pool_table_full,
        (unsigned long long)object_table_full, (unsigned long long)view_table_full);
    pm_unlock(&lock);
}
