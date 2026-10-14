/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_texture.h"
#include "gpu/nvn_alias.h"
#include "gpu/nvn_pool.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <string.h>

/* Texture metadata and pool generations only. Storage layout and contents remain owned by the guest SDK. */
typedef struct {
    u64 key, generation, pool, offset, pool_generation, gpu_address;
    i64 stride;
    u32 flags, target, width, height, depth, levels, format, samples, storage_size, storage_class;
    u8 alive, size_known, address_known, stride_known, storage_class_known, legacy_layout;
} Texture;
#define TEXTURE_SLOTS 32768
static Texture textures[TEXTURE_SLOTS];
typedef struct {
    u64 key, words[8];
    NvnTextureSnapshot snapshot;
} CopiedTexture;
#define COPIED_TEXTURE_SLOTS 8192
static CopiedTexture copied_textures[COPIED_TEXTURE_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, failed, finalized, live, reinitialized, invalid_builders, table_full, legacy_builders;
static u64 no_pool, untracked_pools, offset_outside_pool, unknown_finalizes;
static u64 sizes_known, sizes_unavailable, footprint_outside_pool;
static u64 addresses_known, addresses_unavailable, address_mismatches;
static u64 strides_known, strides_unavailable, storage_classes_known, storage_classes_unavailable;
static u64 stride_zero, stride_positive, stride_negative;
static struct {
    u32 value;
    u64 count;
} class_counts[32];
static u32 class_count;
static u32 view_layout;

void nvn_texture_set_view_layout(u32 first_instruction, u32 second_instruction, u32 third_instruction) {
    u32 layout = 0;
    if (first_instruction == 0xa9007c1f && second_instruction == 0xd65f03c0)
        layout = 16;
    else if (first_instruction == 0x6f00e400 && second_instruction == 0xb900201f && third_instruction == 0xad000000)
        layout = 36;
    else if (first_instruction == 0xb900201f && second_instruction == 0xa9017c1f &&
             third_instruction == 0xa9007c1f)
        layout = 36;
    __atomic_store_n(&view_layout, layout, __ATOMIC_RELEASE);
}

u32 nvn_texture_view_layout(void) { return __atomic_load_n(&view_layout, __ATOMIC_ACQUIRE); }

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

static CopiedTexture* copied_slot(u64 key) {
    u32 home = (u32)((key >> 4) * 2654435761u) & (COPIED_TEXTURE_SLOTS - 1);
    for (u32 step = 0; step < COPIED_TEXTURE_SLOTS; step++) {
        CopiedTexture* copy = &copied_textures[(home + step) & (COPIED_TEXTURE_SLOTS - 1)];
        if (copy->key == key || !copy->key) return copy;
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

int nvn_texture_view_snapshot(u64 ptr, NvnTextureViewSelection* out) {
    *out = (NvnTextureViewSelection){0};
    u32 layout = nvn_texture_view_layout();
    if (!layout || !readable_range(ptr, layout)) return 0;
    u8 bytes[36] = {0};
    for (u32 i = 0; i < layout; i += 4) {
        u32 word = gr32(ptr + i);
        memcpy(bytes + i, &word, 4);
    }
    if (layout == 16) {
        u64 packed;
        memcpy(&packed, bytes, sizeof packed);
        out->flags = (packed >> 56) & 0x3f;
        out->base_level = (packed >> 32) & 0xff;
        out->level_count = (packed >> 40) & 0xff;
        out->first_layer = packed & 0xffff;
        out->layer_count = (packed >> 16) & 0xffff;
        memcpy(&out->format, bytes + 8, 4);
        out->target = (packed >> 48) & 0xff;
        /* The compact view stores the four NVN swizzle enums bytewise and depth mode in its top two bits. */
        out->swizzle_identity = bytes[12] == 2 && bytes[13] == 3 && bytes[14] == 4 && bytes[15] == 5;
        out->depth_mode_depth = !(packed >> 62);
    } else {
        out->flags = bytes[0] & 0x3f;
        out->base_level = bytes[1];
        out->level_count = bytes[2];
        memcpy(&out->first_layer, bytes + 4, 2);
        memcpy(&out->layer_count, bytes + 6, 2);
        memcpy(&out->format, bytes + 8, 4);
        memcpy(&out->target, bytes + 32, 4);
        static const u32 identity[4] = {2, 3, 4, 5};
        out->swizzle_identity = !memcmp(bytes + 12, identity, sizeof identity);
        u32 depth_mode;
        memcpy(&depth_mode, bytes + 28, sizeof depth_mode);
        out->depth_mode_depth = depth_mode == 0;
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
    /* Odyssey places pool/offset/flags at +8/+16/+24, one word before the newer builder. Identify the older
       layout by its live pool at +8; an unrelated pointer there must not displace a valid pool at +16. */
    u64 builder = c->x[1];
    if (!readable_range(builder, 0x40)) return;
    NvnPoolSnapshot pool = {0};
    u64 old_pool = gr64(builder + 8), new_pool = gr64(builder + 0x10);
    call->legacy_layout = old_pool && nvn_pool_snapshot(old_pool, &pool) &&
                          !nvn_pool_snapshot(new_pool, &pool);
    u32 shift = call->legacy_layout ? 8 : 0x10;
    call->pool = gr64(builder + shift);
    call->offset = gr64(builder + shift + 8);
    call->flags = gr32(builder + shift + 0x10);
    call->target = gr32(builder + shift + 0x14);
    call->width = gr32(builder + shift + 0x18);
    call->height = gr32(builder + shift + 0x1c);
    call->depth = gr32(builder + shift + 0x20);
    call->levels = gr32(builder + shift + 0x24);
    call->format = gr32(builder + shift + 0x28);
    call->samples = gr32(builder + shift + 0x2c);
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
                t->stride = call->stride;
                t->stride_known = call->stride_known;
                t->storage_class = call->storage_class;
                t->storage_class_known = call->storage_class_known;
                t->legacy_layout = call->legacy_layout;
                t->alive = 1;
                initialized++;
                if (call->legacy_layout) legacy_builders++;
                nvn_alias_retire(NVN_ALIAS_TEXTURE, t->key);
                if (call->size_known)
                    sizes_known++;
                else
                    sizes_unavailable++;
                if (call->stride_known) {
                    strides_known++;
                    if (call->stride > 0)
                        stride_positive++;
                    else if (call->stride < 0)
                        stride_negative++;
                    else
                        stride_zero++;
                } else
                    strides_unavailable++;
                if (call->storage_class_known) {
                    storage_classes_known++;
                    u32 slot = 0;
                    while (slot < class_count && class_counts[slot].value != call->storage_class) slot++;
                    if (slot == class_count && class_count < 32) {
                        class_counts[slot].value = call->storage_class;
                        class_count++;
                    }
                    if (slot < class_count) class_counts[slot].count++;
                } else
                    storage_classes_unavailable++;
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
    LOG("nvn-texture: initialized=%llu failed=%llu finalized=%llu live=%llu reinitialized=%llu invalid-builders=%llu table-full=%llu legacy-builders=%llu\n",
        (unsigned long long)initialized, (unsigned long long)failed, (unsigned long long)finalized,
        (unsigned long long)live, (unsigned long long)reinitialized, (unsigned long long)invalid_builders,
        (unsigned long long)table_full, (unsigned long long)legacy_builders);
    LOG("nvn-texture: no-pool=%llu untracked-pools=%llu offset-outside-pool=%llu unknown-finalizes=%llu\n",
        (unsigned long long)no_pool, (unsigned long long)untracked_pools, (unsigned long long)offset_outside_pool,
        (unsigned long long)unknown_finalizes);
    LOG("nvn-texture: sizes-known=%llu sizes-unavailable=%llu footprint-outside-pool=%llu\n",
        (unsigned long long)sizes_known, (unsigned long long)sizes_unavailable,
        (unsigned long long)footprint_outside_pool);
    LOG("nvn-texture: addresses-known=%llu unavailable=%llu pool-mismatches=%llu\n",
        (unsigned long long)addresses_known, (unsigned long long)addresses_unavailable,
        (unsigned long long)address_mismatches);
    LOG("nvn-texture-layout: strides-known=%llu unavailable=%llu classes-known=%llu unavailable=%llu\n",
        (unsigned long long)strides_known, (unsigned long long)strides_unavailable,
        (unsigned long long)storage_classes_known, (unsigned long long)storage_classes_unavailable);
    LOG("nvn-texture-layout: stride-zero=%llu positive=%llu negative=%llu\n", (unsigned long long)stride_zero,
        (unsigned long long)stride_positive, (unsigned long long)stride_negative);
    for (u32 i = 0; i < class_count; i++)
        LOG("nvn-texture-class: value=%x count=%llu\n", class_counts[i].value,
            (unsigned long long)class_counts[i].count);
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
        out->stride = t->stride;
        out->stride_known = t->stride_known;
        out->storage_class = t->storage_class;
        out->storage_class_known = t->storage_class_known;
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

static NvnTargetResult resolve_target_snapshot(u64 texture, const NvnTextureSnapshot* texture_snapshot,
                                               const NvnTextureViewSelection* view, NvnTextureTarget* out) {
    const NvnTextureSnapshot t = *texture_snapshot;
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
    *out = (NvnTextureTarget){.texture = texture,
                              .pool = t.pool,
                              .gpu_address = t.gpu_address,
                              .stride = t.stride,
                              .texture_generation = t.generation,
                              .pool_generation = t.pool_generation,
                              .width = t.width,
                              .height = t.height,
                              .depth = t.depth,
                              .levels = t.levels,
                              .format = (selection.flags & 4) ? selection.format : t.format,
                              .storage_format = t.format,
                              .storage_size = t.storage_size,
                              .target = (selection.flags & 32) ? selection.target : t.target,
                              .storage_target = t.target,
                              .samples = t.samples,
                              .flags = t.flags,
                              .storage_class = t.storage_class,
                              .stride_known = t.stride_known,
                              .storage_class_known = t.storage_class_known,
                              .view = selection};
    return NVN_TARGET_RESOLVED;
}

NvnTargetResult nvn_texture_resolve_target(u64 texture, const NvnTextureViewSelection* view, NvnTextureTarget* out) {
    NvnTextureSnapshot t;
    if (!nvn_texture_snapshot(texture, &t)) return NVN_TARGET_MISSING;
    return resolve_target_snapshot(texture, &t, view, out);
}

NvnTargetResult nvn_texture_find_target(u64 gpu_address, u32 width, u32 height,
                                         u32* matches, NvnTextureTarget* out) {
    if (matches) *matches = 0;
    if (!gpu_address || !width || !height || !out) return NVN_TARGET_MISSING;
    u64 texture = 0;
    u32 found = 0;
    Texture first = {0};
    Texture other = {0};
    int equivalent = 1;
    pm_lock(&lock);
    for (u32 i = 0; i < TEXTURE_SLOTS; i++) {
        const Texture* t = &textures[i];
        if (!t->key || !t->alive || !t->address_known || t->gpu_address != gpu_address ||
            t->width != width || t->height != height) continue;
        if (!found) {
            texture = t->key;
            first = *t;
        } else if (t->pool != first.pool || t->offset != first.offset ||
                   t->storage_size != first.storage_size || t->format != first.format ||
                   t->target != first.target || t->depth != first.depth ||
                   t->levels != first.levels || t->samples != first.samples ||
                   t->storage_class != first.storage_class || t->stride != first.stride) {
            equivalent = 0;
            if (!other.key) other = *t;
        }
        found++;
    }
    pm_unlock(&lock);
    if (other.key)
        LOG_LIMIT(6, "nvn-raw-target-ambiguous: gpu=%llx first=%llx/%llx+%llx size=%u format=%x target=%u depth=%u levels=%u samples=%u class=%x stride=%lld other=%llx/%llx+%llx size=%u format=%x target=%u depth=%u levels=%u samples=%u class=%x stride=%lld\n",
                  (unsigned long long)gpu_address,
                  (unsigned long long)first.key, (unsigned long long)first.pool,
                  (unsigned long long)first.offset, first.storage_size, first.format,
                  first.target, first.depth, first.levels, first.samples, first.storage_class,
                  (long long)first.stride,
                  (unsigned long long)other.key, (unsigned long long)other.pool,
                  (unsigned long long)other.offset, other.storage_size, other.format,
                  other.target, other.depth, other.levels, other.samples, other.storage_class,
                  (long long)other.stride);
    if (matches) *matches = found;
    return found && equivalent ? nvn_texture_resolve_target(texture, NULL, out) : NVN_TARGET_MISSING;
}

NvnTargetResult nvn_texture_find_layer_target(u64 gpu_address, u32 width, u32 height,
                                               u32* matches, NvnTextureTarget* out) {
    u32 exact_matches = 0;
    NvnTargetResult exact = nvn_texture_find_target(gpu_address, width, height, &exact_matches, out);
    if (matches) *matches = exact_matches;
    if (exact == NVN_TARGET_RESOLVED || exact_matches) return exact;
    if (!gpu_address || !width || !height || !out) return NVN_TARGET_MISSING;
    u64 texture = 0;
    u32 layer = 0, found = 0;
    Texture first = {0};
    int equivalent = 1;
    pm_lock(&lock);
    for (u32 i = 0; i < TEXTURE_SLOTS; i++) {
        const Texture* t = &textures[i];
        if (!t->key || !t->alive || !t->address_known || !t->size_known || !t->gpu_address ||
            (t->target != 4 && t->target != 8) || t->depth < 2 || !t->storage_size ||
            t->storage_size % t->depth || t->width != width || t->height != height ||
            gpu_address < t->gpu_address) continue;
        u64 layer_stride = t->storage_size / t->depth;
        u64 delta = gpu_address - t->gpu_address;
        if (!layer_stride || delta >= t->storage_size || delta % layer_stride) continue;
        if (!found) {
            texture = t->key;
            layer = (u32)(delta / layer_stride);
            first = *t;
        } else if (t->pool != first.pool || t->offset != first.offset ||
                   t->storage_size != first.storage_size || t->format != first.format ||
                   t->target != first.target || t->depth != first.depth ||
                   t->levels != first.levels || t->samples != first.samples ||
                   t->storage_class != first.storage_class || t->stride != first.stride ||
                   (u32)(delta / layer_stride) != layer) {
            equivalent = 0;
        }
        found++;
    }
    pm_unlock(&lock);
    if (matches) *matches = found;
    if (!found || !equivalent || nvn_texture_resolve_target(texture, NULL, out) != NVN_TARGET_RESOLVED)
        return NVN_TARGET_MISSING;
    out->view.flags = 2;
    out->view.first_layer = layer;
    out->view.layer_count = 1;
    return NVN_TARGET_RESOLVED;
}

NvnTargetResult nvn_texture_resolve_copied_target(u64 texture, const NvnTextureViewSelection* view,
                                                  NvnTextureTarget* out) {
    /* Older SDK clients can memcpy an initialized texture object before SetRenderTargets. Accept a copy only when
       its complete old-layout identity matches one observed Initialize record in the same live pool generation. */
    if (!readable_range(texture, 0x40)) return NVN_TARGET_MISSING;
    u64 words[8];
    memcpy(words, GP(texture), sizeof words);
    u64 pool_key = words[2], offset = words[3];
    NvnPoolSnapshot pool = {0};
    if (!pool_key || !nvn_pool_snapshot(pool_key, &pool)) return NVN_TARGET_MISSING;
    u32 flags = (u32)words[4], target = (u32)(words[4] >> 32);
    u32 width = (u32)words[5], height = (u32)(words[5] >> 32);
    u32 depth = (u32)words[6], levels = (u32)(words[6] >> 32);
    u32 format = (u32)words[7], samples = (u32)(words[7] >> 32);
    NvnTextureSnapshot match = {0};
    int found = 0, ambiguous = 0;
    pm_lock(&lock);
    CopiedTexture* copy = copied_slot(texture);
    if (copy && copy->key == texture && !memcmp(copy->words, words, sizeof words) &&
        copy->snapshot.pool_generation == pool.generation) {
        match = copy->snapshot;
        pm_unlock(&lock);
        return resolve_target_snapshot(texture, &match, view, out);
    }
    for (u32 i = 0; i < TEXTURE_SLOTS; i++) {
        const Texture* t = &textures[i];
        if (!t->key || !t->legacy_layout || !t->address_known || t->pool != pool_key ||
            t->pool_generation != pool.generation || t->offset != offset || t->flags != flags || t->target != target ||
            t->width != width || t->height != height || t->depth != depth || t->levels != levels ||
            t->format != format || t->samples != samples)
            continue;
        if (found && (match.gpu_address != t->gpu_address || match.storage_size != t->storage_size)) {
            ambiguous = 1;
            break;
        }
        match = (NvnTextureSnapshot){.generation = t->generation,
                                     .pool = t->pool,
                                     .pool_generation = t->pool_generation,
                                     .offset = t->offset,
                                     .storage_size = t->storage_size,
                                     .gpu_address = t->gpu_address,
                                     .stride = t->stride,
                                     .stride_known = t->stride_known,
                                     .storage_class = t->storage_class,
                                     .storage_class_known = t->storage_class_known,
                                     .address_known = t->address_known,
                                     .flags = t->flags,
                                     .target = t->target,
                                     .width = t->width,
                                     .height = t->height,
                                     .depth = t->depth,
                                     .levels = t->levels,
                                     .format = t->format,
                                     .samples = t->samples};
        found = 1;
    }
    if (found && !ambiguous && copy) {
        copy->key = texture;
        memcpy(copy->words, words, sizeof words);
        copy->snapshot = match;
    }
    pm_unlock(&lock);
    return found && !ambiguous ? resolve_target_snapshot(texture, &match, view, out) : NVN_TARGET_MISSING;
}
