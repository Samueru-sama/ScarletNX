/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_record.h"
#include "gpu/nvn_command_gap.h"
#include "gpu/nvn_descriptor.h"
#include "gpu/nvn_copy.h"
#include "gpu/nvn_draw.h"
#include "gpu/nvn_dispatch.h"
#include "gpu/nvn_memory_write.h"
#include "gpu/nvn_sampler_descriptor.h"
#include "gpu/nvn_program.h"
#include "gpu/nvn_pool.h"
#include "gpu/nvn_texture.h"
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* Diagnostic sidecar only. The guest SDK still executes every command and owns rendering. */
enum {
    OP_DRAW,
    OP_CLEAR,
    OP_DISPATCH,
    OP_CALL,
    OP_STATE,
    OP_MEMORY,
    OP_TRANSFER,
    OP_BARRIER,
    OP_SYNC,
    OP_OTHER,
    OP_COUNT
};
typedef struct {
    u32 count, capacity;
    NvnDrawCommand commands[];
} NvnDrawBatch;
typedef struct {
    u32 count, capacity;
    NvnClearCommand commands[];
} NvnClearBatch;
typedef struct {
    u32 count, capacity;
    NvnCopyBufferTextureCommand commands[];
} NvnCopyBatch;
typedef struct {
    u32 count, capacity;
    NvnDispatchCommand commands[];
} NvnDispatchBatch;
typedef struct {
    u32 count, capacity;
    NvnMemoryWriteCommand commands[];
} NvnMemoryWriteBatch;
typedef struct {
    u8 kind;
    u8 refs_truncated;
    u16 ref_count;
    const char* name; /* stable name in the resolver's process-lifetime table */
    u64 args[7];      /* x1..x7 at the NVN call boundary */
    u64 refs[8];      /* CallCommands list handles, copied before the guest may reuse its array */
    void* payload;    /* owned snapshot for pointer arguments used by semantic replay */
    u32 storage_cbuf_word;
    u8 storage_cbuf_word_known;
} NvnOp;
typedef struct {
    NvnTextureViewSelection selection;
    u8 valid;
} NvnView;
typedef struct {
    u32 count;
    u8 color_views, depth_view, valid, color_resolved, depth_resolved;
    u64 color[8], depth;
    NvnView color_view[8], depth_view_data;
    NvnTextureTarget color_target[8], depth_target;
} NvnTargets;
typedef struct {
    u32 color[4];
    u32 index, mask;
} NvnClearColor;
typedef struct {
    u32 depth, stencil, depth_mask, stencil_mask;
} NvnClearDepth;
typedef struct {
    i32 region[6];
    NvnView view;
    u8 valid, has_view;
} NvnCopyRegion;
typedef struct {
    NvnTextureTarget source, target;
    i32 source_region[6], target_region[6];
    u8 source_valid, regions_valid;
} NvnTextureCopy;
typedef struct {
    u32 attrib[NVN_DRAW_VERTEX_SLOTS];
    u64 stream[NVN_DRAW_VERTEX_SLOTS];
    u8 count;
} NvnVertexState;
typedef struct {
    u8 first, count, words;
    u32 values[];
} NvnDynamicArray;
typedef struct {
    u64 key;
    u32 record_generation;
    u64 command_cursor, command_low, command_high;
    NvnGapMacroData3 cbuf_macro_data;
    NvnGapMemoryState memory_state;
    u8 cbuf_macro_data_known;
    u32 cb_size, cb_high, cb_low;
    u8 cb_known;
    struct {
        u32 start_high, start_low, end_high, end_low;
        u8 known;
    } gap_vertex[NVN_DRAW_VERTEX_SLOTS];
    u16 gap_vertex_dirty;
    NvnOp* ops;
    u16 count, capacity;
    u8 truncated, opaque, implicit_counted;
    u16 command_memory_count;
    u8 command_prefix_known, command_gaps_safe, command_tail_known;
} NvnList;
#define ACTIVE_SLOTS 8192
#define LIST_SLOTS   32768
/* Odyssey records more than 16,384 operations in one list. Keep the bound within u16 capacity. */
#define MAX_OPS      32768
static NvnList active[ACTIVE_SLOTS], lists[LIST_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u32 next_record_generation = 1;
static u64 began, implicit_begins, ended, ended_opaque, submitted, submitted_found, submitted_missing, submitted_opaque;
static u64 inlined_draws, inlined_uniform_binds, inlined_uniform_unknown, inlined_vertex_binds,
    inlined_vertex_unknown, inlined_gaps, inlined_unparsed;
static u64 cbuf_macro_packets;
static u64 storage_bind_packets, storage_bind_mismatches;
static u64 storage_draw_snapshots, storage_bind_invalid;
static int inlined_draw_replay;
static u64 calls, call_refs, call_ref_missing, call_ref_opaque, truncated_refs;
static u64 active_collisions, list_collisions, truncated_lists, dropped_ops, invalid_arrays, allocation_failures;
static u64 captured[OP_COUNT], ended_ops[OP_COUNT], submitted_ops[OP_COUNT], referenced_ops[OP_COUNT];
typedef struct {
    u64 texture_pool, sampler_pool;
    i64 copy_row_stride, copy_image_stride;
    NvnTargets targets;
    NvnStageProgram program[NVN_DRAW_SHADER_STAGES];
    u8 program_bound;
    u16 vertex_bound;
    NvnBufferRange vertex[NVN_DRAW_VERTEX_SLOTS];
    u32 vertex_attrib_raw[NVN_DRAW_VERTEX_SLOTS];
    u64 vertex_stream_raw[NVN_DRAW_VERTEX_SLOTS];
    u8 vertex_attrib_count, vertex_stream_count;
    u32 channel_mask;
    u8 channel_mask_known;
    u32 color_state;
    u8 color_state_known, blend_state_known;
    u32 blend_state[8][2];
    u32 polygon_state;
    u8 polygon_state_known;
    u32 depth_stencil_state[2];
    u8 depth_stencil_state_known;
    u32 multisample_state[6];
    u8 multisample_state_known;
    u32 viewport[NVN_DRAW_VIEWPORT_SLOTS][4];
    u32 scissor[NVN_DRAW_VIEWPORT_SLOTS][4];
    u32 depth_range[NVN_DRAW_VIEWPORT_SLOTS][2];
    u16 viewport_known, scissor_known, depth_range_known;
    u32 blend_color[4];
    u8 blend_color_known;
    u32 stencil_value_mask[2], stencil_write_mask[2], stencil_ref[2];
    u8 stencil_value_mask_known, stencil_write_mask_known, stencil_ref_known;
    u32 line_width, point_size, alpha_ref;
    u32 polygon_offset[3], depth_bounds[2], sample_mask;
    u8 line_width_known, point_size_known, alpha_ref_known, polygon_offset_known, depth_bounds_known;
    u8 depth_bounds_enabled, sample_mask_known, rasterizer_discard_known, rasterizer_discard;
    u8 depth_clamp_known, depth_clamp;
    NvnBufferRange uniform[NVN_DRAW_SHADER_STAGES][NVN_DRAW_UNIFORM_SLOTS];
    u16 uniform_bound[NVN_DRAW_SHADER_STAGES];
    NvnStorageBinding storage[NVN_DRAW_SHADER_STAGES][NVN_DRAW_STORAGE_SLOTS];
    u16 storage_known[NVN_DRAW_SHADER_STAGES];
    u64 texture[NVN_DRAW_SHADER_STAGES][NVN_DRAW_TEXTURE_SLOTS];
    u32 texture_bound[NVN_DRAW_SHADER_STAGES];
    u64 image[NVN_DRAW_SHADER_STAGES][NVN_DRAW_IMAGE_SLOTS];
    u8 image_bound[NVN_DRAW_SHADER_STAGES];
    u64 render_condition_address;
    u8 render_condition_mode, render_condition_active, render_enabled;
} ReplayState;
typedef struct {
    u64 address, value;
    u8 known;
} ReplayQueryValue;
typedef struct {
    u64 key;
    u64 next_sequence;
    u64 last_unmodeled_write;
    ReplayState state;
    u32 pending_roots, pending_clears;
    u8 observed_clear_only;
    u64 shader_code[8];
    u8 shader_count;
    u64 vertex_probe_code[8];
    u8 vertex_probe_count;
    NvnClearBatch* clears;
    NvnCopyBatch* copies;
    NvnDrawBatch* draws;
    NvnDispatchBatch* dispatches;
    NvnMemoryWriteBatch* memory_writes;
    NvnStorageSeed* storage_seeds;
    u64 storage_seed_bytes;
    NvnUniformUpdate* uniform_updates;
    u64 uniform_update_bytes;
    u8 uniform_update_failed;
    ReplayQueryValue query_values[256];
    u16 query_value_count;
} ReplayQueue;
#define REPLAY_QUEUE_SLOTS 1024
static ReplayQueue replay_queues[REPLAY_QUEUE_SLOTS];
static int descriptor_replay;
static int readable_range(u64 addr, u64 size);
static int shader_transport;
static int clear_execute;
static int copy_execute;
static int draw_prepare;
static int dispatch_execute;
#define NVN_DISPATCH_SEED_BUDGET (16u << 20)
#define NVN_UNIFORM_UPDATE_BUDGET (16u << 20)
static int texture_load;
static int attachment_prepare;
static int shader_materialize;
static int pipeline_prepare;
static u64 shader_staged, shader_flushed, shader_stage_full;
static u64 replay_roots, replay_lists, replay_opaque, replay_missing_lists, replay_truncated_refs, replay_depth_limit;
static u64 replay_complete_opaque;
static u64 replay_binds, replay_resolved, replay_missing_pool, replay_missing_descriptor, replay_stale;
static u64 replay_missing_zero_handle, replay_missing_nonzero_handle;
static u64 replay_sampler_resolved, replay_sampler_missing_pool, replay_sampler_missing_descriptor,
    replay_sampler_stale;
static u64 replay_program_binds, replay_program_resolved, replay_program_missing, replay_program_unconfigured;
static u64 replay_program_refs_known, replay_program_shader_refs;
static u64 replay_queue_full;
static u64 clear_snapshots, clear_snapshot_failures, depth_snapshots, target_snapshots, target_snapshot_failures;
static u64 view_snapshots, view_snapshot_failures;
static u64 view_flags[6];
static u64 replay_color_clears, replay_color_target_resolved, replay_color_missing_target, replay_color_view_unknown,
    replay_color_stale_target;
static u64 replay_texture_clears, replay_texture_target_resolved, replay_texture_missing_target;
static u64 replay_depth_clears, replay_depth_target_resolved, replay_depth_missing_target, replay_depth_view_unknown,
    replay_depth_stale_target;
static u64 replay_target_stale_pool, replay_target_no_address, replay_target_bad_view;
static u64 clear_staged, clear_flushed, clear_stage_full, clear_issued, clear_unsupported, clear_image_full,
    clear_array_issued, clear_cube_issued, clear_mip_issued;
static u64 preflight_batches, preflight_clear_only, preflight_mixed, preflight_no_clears, preflight_clear_commands;
static u64 vertex_range_snapshots, vertex_range_snapshot_failures, vertex_bind_invalid;
static u64 draw_arrays, draw_elements, draw_base_vertex, draw_instanced, draw_other, draw_with_program,
    draw_with_shader_refs;
static u64 draw_with_vertex, draw_without_vertex, draw_index_invalid, draw_staged, draw_stage_full, draw_flushed;
static u64 draw_viewport_known, draw_scissor_known, draw_depth_range_known;
static u64 draw_blend_color_known, draw_stencil_value_mask_known, draw_stencil_write_mask_known, draw_stencil_ref_known;
static u64 draw_line_width_known, draw_polygon_offset_known, draw_depth_bounds_known, draw_sample_mask_known;
static u64 draw_rasterizer_discard_known, draw_depth_clamp_known;
static u64 draw_prepared, draw_prepare_unsupported, draw_shared_target, draw_target_no_surf, draw_target_format,
    draw_target_size, draw_target_view;
static u64 draw_attachment_ready, draw_attachment_failed;
static u64 draw_shader_module_ready, draw_shader_module_failed;
static u64 draw_zero_bind_textureless, draw_zero_bind_textured;
static u64 draw_zero_bind_all_bindless;
static u64 draw_texture_unused;
static u64 draw_texture_slots_mapped;
static u64 draw_vertex_format_ready, draw_vertex_format_failed;
static u64 draw_pipeline_ready, draw_pipeline_failed;
static u64 draw_sampler_ready, draw_sampler_missing, draw_sampler_failed;
static u64 draw_texture_header_ready, draw_texture_header_missing, draw_texture_header_unsupported;
static u64 draw_texture_image_ready, draw_texture_image_failed, draw_texture_image_semantic;
static u64 draw_texture_image_depth, draw_texture_image_missing, draw_texture_image_stale;
static u64 copy_snapshots, copy_snapshot_failures, copy_staged, copy_stage_full, copy_flushed, copy_issued,
    copy_unsupported;
static u64 scoped_copy_staged, scoped_copy_issued, scoped_copy_unsupported;
static u64 texture_blit_issued;
static u64 uniform_range_snapshots, uniform_range_snapshot_failures, uniform_bind_invalid;
static u64 draw_with_uniform, draw_without_uniform;
static u64 texture_handle_snapshots, texture_handle_snapshot_failures, texture_bind_invalid;
static u64 draw_with_texture, draw_without_texture;
static u64 image_handle_snapshots, image_handle_snapshot_failures, image_bind_invalid;
static u64 draw_with_image, draw_image_descriptors_complete, draw_image_descriptors_incomplete;
static u64 draw_image_targets_complete, draw_image_targets_incomplete;
static u64 draw_image_storage_ready;
static u64 draw_texture_descriptors_complete, draw_texture_descriptors_incomplete;
static u64 draw_texture_tics_complete, draw_texture_tics_incomplete;
static u64 draw_texture_views_complete, draw_texture_views_incomplete;
static u64 draw_sampler_descriptors_complete, draw_sampler_descriptors_incomplete;
static u64 draw_sampler_tscs_complete, draw_sampler_tscs_incomplete;
static u64 draw_sampler_filter_wrap_complete, draw_sampler_filter_wrap_incomplete;
static u64 program_mask_zero, program_mask_invalid, draw_vertex_program, draw_fragment_program;
static u64 vertex_attrib_snapshots, vertex_stream_snapshots, vertex_state_snapshot_failures;
static u64 draw_with_attrib_state, draw_with_stream_state;
static u64 draw_enabled_attribs, draw_bad_attrib_stream;
static u64 draw_zero_count, draw_zero_instances;
static u64 draw_targets_complete, draw_targets_incomplete, draw_targets_none;

static u32 slot(u64 key, u32 mask) { return (u32)((key >> 4) * 2654435761u) & mask; }

static NvnList* active_slot(u64 key, int create) {
    if (!key || key == UINT64_MAX) return NULL;
    u32 home = slot(key, ACTIVE_SLOTS - 1);
    NvnList* tombstone = NULL;
    for (u32 step = 0; step < ACTIVE_SLOTS; step++) {
        NvnList* r = &active[(home + step) & (ACTIVE_SLOTS - 1)];
        if (r->key == key) return r;
        if (r->key == UINT64_MAX && !tombstone) tombstone = r;
        if (!r->key) return create ? (tombstone ? tombstone : r) : NULL;
    }
    if (!create) return NULL;
    if (tombstone) return tombstone;
    active_collisions++; /* bounded diagnostic storage is full */
    return &active[home];
}

/* Preserve secondary lists that hash to the same slot until CallCommands references them. */
static NvnList* list_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = slot(key, LIST_SLOTS - 1);
    for (u32 step = 0; step < LIST_SLOTS; step++) {
        NvnList* r = &lists[(home + step) & (LIST_SLOTS - 1)];
        if (r->key == key) return r;
        if (!r->key) return create ? r : NULL;
    }
    if (!create) return NULL;
    list_collisions++; /* fixed storage is full: evict one diagnostic record */
    return &lists[home];
}

void nvn_record_enable_descriptor_replay(void) { descriptor_replay = 1; }
void nvn_record_enable_shader_transport(void) { shader_transport = 1; }
void nvn_record_enable_clear_execute(void) { clear_execute = 1; }
void nvn_record_enable_copy_execute(void) { copy_execute = 1; }
void nvn_record_enable_draw_prepare(void) { draw_prepare = 1; }
void nvn_record_enable_dispatch_execute(void) { dispatch_execute = 1; }
void nvn_record_enable_texture_load(void) {
    draw_prepare = 1;
    texture_load = 1;
}
void nvn_record_enable_attachment_prepare(void) {
    draw_prepare = 1;
    attachment_prepare = 1;
}
void nvn_record_enable_shader_materialize(void) {
    draw_prepare = 1;
    shader_materialize = 1;
}
void nvn_record_enable_pipeline_prepare(void) {
    draw_prepare = 1;
    attachment_prepare = 1;
    shader_materialize = 1;
    pipeline_prepare = 1;
}
void nvn_record_enable_inlined_draws(void) { inlined_draw_replay = 1; }

static ReplayQueue* replay_queue_slot(u64 key) {
    if (!key) return NULL;
    u32 home = slot(key, REPLAY_QUEUE_SLOTS - 1);
    for (u32 step = 0; step < REPLAY_QUEUE_SLOTS; step++) {
        ReplayQueue* q = &replay_queues[(home + step) & (REPLAY_QUEUE_SLOTS - 1)];
        if (q->key == key || !q->key) return q;
    }
    return NULL;
}

static ReplayQueryValue* replay_query_value(ReplayQueue* queue, u64 address, int seed) {
    for (u32 i = 0; i < queue->query_value_count; i++)
        if (queue->query_values[i].address == address) return &queue->query_values[i];
    if (queue->query_value_count == 256) return NULL;
    ReplayQueryValue* value = &queue->query_values[queue->query_value_count++];
    *value = (ReplayQueryValue){.address = address};
    if (seed && address <= UINT64_MAX - 8) {
        const u8* host = gmem_ptr_untracked(address, 8);
        if (host) {
            memcpy(&value->value, host, 8);
            value->known = 1;
        }
    }
    return value;
}

static void replay_query_write(ReplayQueue* queue, u64 address, u32 size, u8 kind,
                               u32 payload, int payload_known) {
    if (!queue || !address || !size) return;
    for (u32 i = 0; i < queue->query_value_count; i++) {
        ReplayQueryValue* entry = &queue->query_values[i];
        if (address < entry->address + 8 && entry->address < address + size) entry->known = 0;
    }
    if (kind == NVN_GAP_WRITE_PULLER && size == 16 && payload_known) {
        ReplayQueryValue* value = replay_query_value(queue, address, 0);
        if (value) {
            value->value = payload;
            value->known = 1;
        }
    }
}

static int replay_render_enabled(const ReplayState* state, ReplayQueue* queue) {
    if (state->render_condition_active == 1) return state->render_enabled;
    if (state->render_condition_active != 2 || !queue ||
        state->render_condition_address > UINT64_MAX - 24) return 1;
    ReplayQueryValue* lhs = replay_query_value(queue, state->render_condition_address, 1);
    ReplayQueryValue* rhs = replay_query_value(queue, state->render_condition_address + 16, 1);
    if (!lhs || !rhs || !lhs->known || !rhs->known) return 1; /* unknown: retain the draw */
    return state->render_condition_mode ? lhs->value != rhs->value : lhs->value == rhs->value;
}

static const NvnStorageSeed* replay_storage_seed(ReplayQueue* queue, NvnBufferRange range) {
    if (!queue || !range.address || !range.size || range.size > (16u << 20) - 3) return NULL;
    u32 size = (u32)((range.size + 3) & ~3ull);
    const u8* host = gmem_ptr_untracked(range.address, size);
    if (!host) return NULL;
    for (const NvnStorageSeed* seed = queue->storage_seeds; seed; seed = seed->next)
        if (seed->address == range.address && seed->size == size && seed->guest_host == host &&
            !memcmp(seed->bytes, host, size)) return seed;
    if (queue->storage_seed_bytes > NVN_DISPATCH_SEED_BUDGET - size) return NULL;
    NvnStorageSeed* seed = malloc(sizeof *seed + size);
    if (!seed) {
        allocation_failures++;
        return NULL;
    }
    seed->address = range.address;
    seed->size = size;
    seed->guest_host = host;
    memcpy(seed->bytes, host, size);
    seed->next = queue->storage_seeds;
    queue->storage_seeds = seed;
    queue->storage_seed_bytes += size;
    return seed;
}

static void stage_clear(ReplayQueue* queue, NvnClearCommand command) {
    NvnClearBatch* batch = queue->clears;
    if (!batch || batch->count == batch->capacity) {
        u32 capacity = batch ? batch->capacity * 2 : 8;
        if (capacity > 4096) capacity = 4096;
        if (!batch || batch->capacity < capacity) {
            NvnClearBatch* grown = realloc(batch, sizeof *batch + (size_t)capacity * sizeof command);
            if (grown) {
                if (!batch) grown->count = 0;
                grown->capacity = capacity;
                queue->clears = batch = grown;
            }
        }
    }
    if (!batch || batch->count == batch->capacity) {
        clear_stage_full++;
        return;
    }
    command.sequence = queue->next_sequence++;
    batch->commands[batch->count++] = command;
    clear_staged++;
}

static int stage_copy(ReplayQueue* queue, NvnCopyBufferTextureCommand command) {
    NvnCopyBatch* batch = queue->copies;
    if (!batch || batch->count == batch->capacity) {
        u32 capacity = batch ? batch->capacity * 2 : 8;
        if (capacity > 4096) capacity = 4096;
        if (!batch || batch->capacity < capacity) {
            NvnCopyBatch* grown = realloc(batch, sizeof *batch + (size_t)capacity * sizeof command);
            if (grown) {
                if (!batch) grown->count = 0;
                grown->capacity = capacity;
                queue->copies = batch = grown;
            }
        }
    }
    if (!batch || batch->count == batch->capacity) {
        copy_stage_full++;
        return 0;
    }
    command.sequence = queue->next_sequence++;
    batch->commands[batch->count++] = command;
    copy_staged++;
    return 1;
}

static int stage_memory_write(ReplayQueue* queue, u64 address, u32 size, u8 kind) {
    NvnMemoryWriteBatch* batch = queue->memory_writes;
    if (!batch || batch->count == batch->capacity) {
        u32 capacity = batch ? batch->capacity * 2 : 8;
        if (capacity > 4096) capacity = 4096;
        if (!batch || batch->capacity < capacity) {
            NvnMemoryWriteBatch* grown = realloc(batch, sizeof *batch + (size_t)capacity * sizeof grown->commands[0]);
            if (grown) {
                if (!batch) grown->count = 0;
                grown->capacity = capacity;
                queue->memory_writes = batch = grown;
            }
        }
    }
    u64 prior = queue->last_unmodeled_write;
    u64 sequence = queue->next_sequence++;
    queue->last_unmodeled_write = sequence;
    if (!batch || batch->count == batch->capacity) return 0;
    batch->commands[batch->count++] = (NvnMemoryWriteCommand){sequence, prior, address, size, kind};
    return 1;
}

static void replay_dynamic_array(const NvnOp* op, u32 first, u32 count, u32* dst, u32 words, u16* known) {
    if (first > NVN_DRAW_VIEWPORT_SLOTS || count > NVN_DRAW_VIEWPORT_SLOTS - first) {
        *known = 0;
        return;
    }
    u16 mask = (u16)(((1u << count) - 1) << first);
    const NvnDynamicArray* array = op->payload;
    if (!array || array->first != first || array->count != count || array->words != words) {
        *known &= ~mask;
        return;
    }
    memcpy(dst + first * words, array->values, (size_t)count * words * sizeof(u32));
    *known |= mask;
}

static void replay_stencil_faces(const NvnOp* op, u32 values[2], u8* known) {
    u32 faces = (u32)op->args[0];
    if (!faces) return;
    if (faces & ~3u) {
        *known = 0;
        return;
    }
    if (faces & 1) values[0] = (u32)op->args[1];
    if (faces & 2) values[1] = (u32)op->args[1];
    *known |= (u8)faces;
}

static void replay_float_args(const NvnOp* op, u32* values, u32 count, u8* known) {
    *known = op->payload != NULL;
    if (*known) memcpy(values, op->payload, (size_t)count * sizeof(u32));
}

static NvnTargetResult replay_target(u64 texture, const NvnTextureViewSelection* view,
                                     const NvnTextureTarget* captured, int captured_valid, NvnTextureTarget* out) {
    if (!captured_valid) return nvn_texture_resolve_target(texture, view, out);
    NvnPoolSnapshot pool = {0};
    if (!nvn_pool_snapshot(captured->pool, &pool) || pool.generation != captured->pool_generation)
        return NVN_TARGET_STALE_POOL;
    *out = *captured;
    return NVN_TARGET_RESOLVED;
}

/* Visible calls have their own write markers. An opaque list needs an additional marker only when its hidden
   command-memory span may contain writes or when calls preceded the observed command-memory allocation. */
static int list_command_memory_complete(const NvnList* list) {
    if (!list->opaque || list->truncated || list->command_memory_count != 1 ||
        !list->command_prefix_known || !list->command_gaps_safe || !list->command_tail_known || !list->count)
        return 0;
    int saw_command_memory = 0;
    for (u32 i = 0; i < list->count; i++) {
        if (list->ops[i].name && !strcmp(list->ops[i].name, "nvnCommandBufferAddCommandMemory"))
            saw_command_memory = 1;
        else if (!saw_command_memory && list->ops[i].kind != OP_MEMORY) return 0;
    }
    return 1;
}

/* Walk a diagnostic list in GPU command order. Secondary lists inherit both descriptor-pool selections. */
static void replay_list(u64 handle, ReplayState* state, ReplayQueue* queue, u32 depth) {
    if (depth == 32) {
        replay_depth_limit++;
        if (queue) {
            queue->observed_clear_only = 0;
            queue->last_unmodeled_write = queue->next_sequence++;
        }
        return;
    }
    NvnList* list = list_slot(handle, 0);
    if (!list) {
        replay_missing_lists++;
        if (queue) {
            queue->observed_clear_only = 0;
            queue->last_unmodeled_write = queue->next_sequence++;
        }
        return;
    }
    replay_lists++;
    if (list->opaque) replay_opaque++;
    int complete_opaque = list_command_memory_complete(list);
    replay_complete_opaque += complete_opaque;
    if (queue && (list->truncated || (list->opaque && !complete_opaque))) {
        queue->observed_clear_only = 0;
        queue->last_unmodeled_write = queue->next_sequence++;
    }
    for (u32 i = 0; i < list->count; i++) {
        const NvnOp* op = &list->ops[i];
        u64 prior_unmodeled_write = queue ? queue->last_unmodeled_write : 0;
        int render_clear_disabled = queue &&
            (!strcmp(op->name, "nvnCommandBufferClearColor") ||
             !strcmp(op->name, "nvnCommandBufferClearDepthStencil")) &&
            !replay_render_enabled(state, queue);
        if (!strcmp(op->name, "nvnCommandBufferUpdateUniformBuffer")) {
            const char* dispatch_trace = getenv("SNX_NVN_DISPATCH_TRACE");
            if (dispatch_trace && !strcmp(dispatch_trace, "1"))
                LOG("nvn-uniform-update: seq=%llu args=%llx,%llx,%llx,%llx,%llx\n",
                    (unsigned long long)(queue ? queue->next_sequence : 0),
                    (unsigned long long)op->args[0], (unsigned long long)op->args[1],
                    (unsigned long long)op->args[2], (unsigned long long)op->args[3],
                    (unsigned long long)op->args[4]);
            if (queue) {
                u64 size = op->args[3], address = op->args[0], offset = op->args[2];
                if (!op->payload || size > NVN_UNIFORM_UPDATE_BUDGET - sizeof(NvnUniformUpdate) ||
                    queue->uniform_update_bytes > NVN_UNIFORM_UPDATE_BUDGET - sizeof(NvnUniformUpdate) - size ||
                    address > UINT64_MAX - offset || address + offset > UINT64_MAX - size) {
                    queue->uniform_update_failed = 1;
                    queue->last_unmodeled_write = queue->next_sequence++;
                } else {
                    NvnUniformUpdate* update = malloc(sizeof *update + (size_t)size);
                    if (!update) {
                        allocation_failures++;
                        queue->uniform_update_failed = 1;
                        queue->last_unmodeled_write = queue->next_sequence++;
                    } else {
                        update->address = address + offset;
                        update->size = (u32)size;
                        memcpy(update->bytes, op->payload, (size_t)size);
                        update->next = queue->uniform_updates;
                        queue->uniform_updates = update;
                        queue->uniform_update_bytes += sizeof *update + size;
                        if (!stage_memory_write(queue, update->address, update->size,
                                                NVN_GAP_WRITE_UNIFORM))
                            queue->uniform_update_failed = 1;
                    }
                }
            }
        }
        /* A captured uniform update has its own bounded write marker. Its payload is applied to
           later direct draws without modifying the guest buffer while GPFIFO remains authoritative. */
        if (queue &&
            ((op->kind == OP_CLEAR && !render_clear_disabled) ||
             (op->kind == OP_TRANSFER && strcmp(op->name, "nvnCommandBufferUpdateUniformBuffer") &&
              strcmp(op->name, "nvnInlinedMemoryWrite")) ||
             op->kind == OP_DISPATCH || op->kind == OP_OTHER)) {
            queue->last_unmodeled_write = queue->next_sequence++;
        }
        if (queue && op->kind != OP_MEMORY && strcmp(op->name, "nvnCommandBufferSetRenderTargets") &&
            strcmp(op->name, "nvnCommandBufferClearColor") && strcmp(op->name, "nvnCommandBufferClearTexture") &&
            strcmp(op->name, "nvnCommandBufferClearDepthStencil") &&
            strcmp(op->name, "nvnCommandBufferCallCommands"))
            queue->observed_clear_only = 0;
        if (!strcmp(op->name, "nvnInlinedMemoryWrite")) {
            if (queue) {
                stage_memory_write(queue, op->args[0], (u32)op->args[1], (u8)op->args[2]);
                replay_query_write(queue, op->args[0], (u32)op->args[1], (u8)op->args[2],
                                   (u32)op->args[3], !!op->args[4]);
            }
            continue;
        }
        if (!strcmp(op->name, "nvnCommandBufferSetCopyRowStride")) {
            state->copy_row_stride = (i64)op->args[0];
        } else if (!strcmp(op->name, "nvnCommandBufferSetCopyImageStride")) {
            state->copy_image_stride = (i64)op->args[0];
        } else if (!strcmp(op->name, "nvnCommandBufferSetViewport")) {
            for (u32 j = 0; j < 4; j++) {
                float value = (float)(i32)op->args[j];
                memcpy(&state->viewport[0][j], &value, sizeof value);
            }
            state->viewport_known |= 1;
        } else if (!strcmp(op->name, "nvnCommandBufferSetViewports")) {
            replay_dynamic_array(op, (u32)op->args[0], (u32)op->args[1], &state->viewport[0][0], 4,
                                 &state->viewport_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetScissor")) {
            for (u32 j = 0; j < 4; j++) state->scissor[0][j] = (u32)op->args[j];
            state->scissor_known |= 1;
        } else if (!strcmp(op->name, "nvnCommandBufferSetScissors")) {
            replay_dynamic_array(op, (u32)op->args[0], (u32)op->args[1], &state->scissor[0][0], 4,
                                 &state->scissor_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetDepthRange")) {
            replay_dynamic_array(op, 0, 1, &state->depth_range[0][0], 2, &state->depth_range_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetDepthRanges")) {
            replay_dynamic_array(op, (u32)op->args[0], (u32)op->args[1], &state->depth_range[0][0], 2,
                                 &state->depth_range_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetBlendColor")) {
            if (op->payload) {
                memcpy(state->blend_color, op->payload, sizeof state->blend_color);
                state->blend_color_known = 1;
            } else
                state->blend_color_known = 0;
        } else if (!strcmp(op->name, "nvnCommandBufferSetStencilValueMask")) {
            replay_stencil_faces(op, state->stencil_value_mask, &state->stencil_value_mask_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetStencilMask")) {
            replay_stencil_faces(op, state->stencil_write_mask, &state->stencil_write_mask_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetStencilRef")) {
            replay_stencil_faces(op, state->stencil_ref, &state->stencil_ref_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetLineWidth")) {
            replay_float_args(op, &state->line_width, 1, &state->line_width_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetPointSize")) {
            replay_float_args(op, &state->point_size, 1, &state->point_size_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetAlphaRef")) {
            replay_float_args(op, &state->alpha_ref, 1, &state->alpha_ref_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetPolygonOffsetClamp")) {
            replay_float_args(op, state->polygon_offset, 3, &state->polygon_offset_known);
        } else if (!strcmp(op->name, "nvnCommandBufferSetDepthBounds")) {
            replay_float_args(op, state->depth_bounds, 2, &state->depth_bounds_known);
            state->depth_bounds_enabled = !!op->args[0];
        } else if (!strcmp(op->name, "nvnCommandBufferSetSampleMask")) {
            state->sample_mask = (u32)op->args[0];
            state->sample_mask_known = 1;
        } else if (!strcmp(op->name, "nvnCommandBufferSetRasterizerDiscard")) {
            state->rasterizer_discard = !!op->args[0];
            state->rasterizer_discard_known = 1;
        } else if (!strcmp(op->name, "nvnCommandBufferSetDepthClamp")) {
            state->depth_clamp = !!op->args[0];
            state->depth_clamp_known = 1;
        } else if (!strcmp(op->name, "nvnCommandBufferCopyTextureToTexture")) {
            const NvnTextureCopy* captured = op->payload;
            NvnPoolSnapshot pool = {0};
            if (copy_execute && queue && captured && captured->target.storage_size &&
                nvn_pool_snapshot(captured->target.pool, &pool) &&
                pool.generation == captured->target.pool_generation) {
                /* Execute an eligible blit, or retain only its destination as an unknown write. */
                NvnCopyBufferTextureCommand packet = {.target = captured->target,
                                                      .unknown_marker = queue->last_unmodeled_write,
                                                      .prior_unmodeled_write = prior_unmodeled_write,
                                                      .scoped_texture_write = 1};
                if (captured->source_valid && captured->regions_valid && !op->args[6] &&
                    nvn_pool_snapshot(captured->source.pool, &pool) &&
                    pool.generation == captured->source.pool_generation) {
                    packet.source_target = captured->source;
                    memcpy(packet.source_region, captured->source_region, sizeof packet.source_region);
                    memcpy(packet.target_region, captured->target_region, sizeof packet.target_region);
                    packet.texture_blit = 1;
                }
                scoped_copy_staged += stage_copy(queue, packet);
            }
        } else if (!strcmp(op->name, "nvnCommandBufferCopyBufferToTexture")) {
            const NvnCopyRegion* copy = op->payload;
            if (copy_execute && queue && copy && copy->valid) {
                NvnTextureTarget target;
                const NvnTextureViewSelection* view = copy->has_view ? &copy->view.selection : NULL;
                if (nvn_texture_resolve_target(op->args[1], view, &target) == NVN_TARGET_RESOLVED) {
                    NvnCopyBufferTextureCommand packet = {.target = target,
                                                          .source = op->args[0],
                                                          .row_stride = state->copy_row_stride,
                                                          .image_stride = state->copy_image_stride,
                                                          .x = copy->region[0],
                                                          .y = copy->region[1],
                                                          .z = copy->region[2],
                                                          .width = copy->region[3],
                                                          .height = copy->region[4],
                                                          .depth = copy->region[5],
                                                          .flags = (u32)op->args[4]};
                    stage_copy(queue, packet);
                }
            }
        } else if (!strcmp(op->name, "nvnCommandBufferSetRenderTargets")) {
            if (op->payload)
                state->targets = *(const NvnTargets*)op->payload;
            else
                memset(&state->targets, 0, sizeof state->targets);
        } else if (!strcmp(op->name, "nvnCommandBufferSetRenderEnable")) {
            state->render_condition_active = 1;
            state->render_enabled = !!op->args[0];
        } else if (!strcmp(op->name, "nvnCommandBufferSetRenderEnableConditional")) {
            state->render_condition_active = op->args[0] <= 1 ? 2 : 0;
            state->render_condition_mode = (u8)op->args[0];
            state->render_condition_address = op->args[1];
        } else if (!strcmp(op->name, "nvnCommandBufferClearTexture")) {
            replay_texture_clears++;
            const NvnClearCommand* clear = op->payload;
            if (!clear || !clear->target.gpu_address) {
                replay_texture_missing_target++;
                if (queue) queue->observed_clear_only = 0;
            } else {
                replay_texture_target_resolved++;
                if (queue) queue->pending_clears++;
                if (clear_execute && queue) {
                    NvnClearCommand packet = *clear;
                    packet.unknown_marker = queue->last_unmodeled_write;
                    packet.prior_unmodeled_write = prior_unmodeled_write;
                    stage_clear(queue, packet);
                }
            }
        } else if (!strcmp(op->name, "nvnCommandBufferClearColor")) {
            replay_color_clears++;
            const NvnClearColor* clear = op->payload;
            if (!clear || !state->targets.valid || clear->index >= state->targets.count ||
                !state->targets.color[clear->index]) {
                replay_color_missing_target++;
                if (queue) queue->observed_clear_only = 0;
            } else if ((state->targets.color_views & (1u << clear->index)) &&
                       !state->targets.color_view[clear->index].valid) {
                replay_color_view_unknown++;
                if (queue) queue->observed_clear_only = 0;
            } else {
                const NvnView* view = &state->targets.color_view[clear->index];
                NvnTextureTarget target;
                NvnTargetResult result = replay_target(
                    state->targets.color[clear->index],
                    (state->targets.color_views & (1u << clear->index)) ? &view->selection : NULL,
                    &state->targets.color_target[clear->index],
                    !!(state->targets.color_resolved & (1u << clear->index)), &target);
                if (result == NVN_TARGET_RESOLVED) {
                    replay_color_target_resolved++;
                    if (queue) queue->pending_clears++;
                    if (clear_execute && queue && replay_render_enabled(state, queue)) {
                        NvnClearCommand packet = {.kind = NVN_CLEAR_COLOR, .target = target, .mask = clear->mask};
                        packet.unknown_marker = queue->last_unmodeled_write;
                        packet.prior_unmodeled_write = prior_unmodeled_write;
                        memcpy(packet.color, clear->color, sizeof packet.color);
                        stage_clear(queue, packet);
                    }
                } else if (result == NVN_TARGET_STALE_POOL)
                    replay_target_stale_pool++;
                else if (result == NVN_TARGET_NO_ADDRESS)
                    replay_target_no_address++;
                else if (result == NVN_TARGET_BAD_VIEW)
                    replay_target_bad_view++;
                else
                    replay_color_stale_target++;
                if (queue && result != NVN_TARGET_RESOLVED) queue->observed_clear_only = 0;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferClearDepthStencil")) {
            replay_depth_clears++;
            if (!op->payload || !state->targets.valid || !state->targets.depth) {
                replay_depth_missing_target++;
                if (queue) queue->observed_clear_only = 0;
            } else if (state->targets.depth_view && !state->targets.depth_view_data.valid) {
                replay_depth_view_unknown++;
                if (queue) queue->observed_clear_only = 0;
            } else {
                NvnTextureTarget target;
                NvnTargetResult result = replay_target(
                    state->targets.depth, state->targets.depth_view ? &state->targets.depth_view_data.selection : NULL,
                    &state->targets.depth_target, state->targets.depth_resolved, &target);
                if (result == NVN_TARGET_RESOLVED) {
                    replay_depth_target_resolved++;
                    if (queue) queue->pending_clears++;
                    if (clear_execute && queue && replay_render_enabled(state, queue)) {
                        const NvnClearDepth* clear = op->payload;
                        NvnClearCommand packet = {.kind = NVN_CLEAR_DEPTH_STENCIL,
                                                  .target = target,
                                                  .depth_bits = clear->depth,
                                                  .stencil = clear->stencil,
                                                  .depth_mask = clear->depth_mask,
                                                  .stencil_mask = clear->stencil_mask};
                        packet.unknown_marker = queue->last_unmodeled_write;
                        packet.prior_unmodeled_write = prior_unmodeled_write;
                        stage_clear(queue, packet);
                    }
                } else if (result == NVN_TARGET_STALE_POOL)
                    replay_target_stale_pool++;
                else if (result == NVN_TARGET_NO_ADDRESS)
                    replay_target_no_address++;
                else if (result == NVN_TARGET_BAD_VIEW)
                    replay_target_bad_view++;
                else
                    replay_depth_stale_target++;
                if (queue && result != NVN_TARGET_RESOLVED) queue->observed_clear_only = 0;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferSetTexturePool")) {
            state->texture_pool = op->args[0];
        } else if (!strcmp(op->name, "nvnCommandBufferSetSamplerPool")) {
            state->sampler_pool = op->args[0];
        } else if (!strcmp(op->name, "nvnCommandBufferBindTexture")) {
            replay_binds++;
            u32 stage = (u32)op->args[0], index = (u32)op->args[1];
            if (stage >= NVN_DRAW_SHADER_STAGES || index >= NVN_DRAW_TEXTURE_SLOTS)
                texture_bind_invalid++;
            else {
                state->texture[stage][index] = op->args[2];
                state->texture_bound[stage] |= 1u << index;
            }
            if (!state->texture_pool) {
                replay_missing_pool++;
            } else {
                int result = nvn_descriptor_check(state->texture_pool, op->args[2]);
                if (result == 1)
                    replay_resolved++;
                else if (result == -1)
                    replay_stale++;
                else {
                    replay_missing_descriptor++;
                    if (op->args[2])
                        replay_missing_nonzero_handle++;
                    else
                        replay_missing_zero_handle++;
                }
            }
            if (!state->sampler_pool) {
                replay_sampler_missing_pool++;
            } else {
                int result = nvn_sampler_descriptor_check(state->sampler_pool, op->args[2]);
                if (result == 1)
                    replay_sampler_resolved++;
                else if (result == -1)
                    replay_sampler_stale++;
                else
                    replay_sampler_missing_descriptor++;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindTextures")) {
            u32 stage = (u32)op->args[0], first = (u32)op->args[1], count = (u32)op->args[2];
            if (stage >= NVN_DRAW_SHADER_STAGES || first >= NVN_DRAW_TEXTURE_SLOTS ||
                count > NVN_DRAW_TEXTURE_SLOTS - first || !op->payload) {
                texture_bind_invalid++;
                if (stage < NVN_DRAW_SHADER_STAGES) state->texture_bound[stage] = 0;
            } else {
                const u64* handles = op->payload;
                for (u32 j = 0; j < count; j++) {
                    state->texture[stage][first + j] = handles[j];
                    state->texture_bound[stage] |= 1u << (first + j);
                }
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindImage")) {
            u32 stage = (u32)op->args[0], index = (u32)op->args[1];
            if (stage >= NVN_DRAW_SHADER_STAGES || index >= NVN_DRAW_IMAGE_SLOTS)
                image_bind_invalid++;
            else {
                state->image[stage][index] = op->args[2];
                state->image_bound[stage] |= (u8)(1u << index);
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindImages")) {
            u32 stage = (u32)op->args[0], first = (u32)op->args[1], count = (u32)op->args[2];
            if (stage >= NVN_DRAW_SHADER_STAGES || first >= NVN_DRAW_IMAGE_SLOTS ||
                count > NVN_DRAW_IMAGE_SLOTS - first || (count && !op->payload)) {
                image_bind_invalid++;
                if (stage < NVN_DRAW_SHADER_STAGES) state->image_bound[stage] = 0;
            } else {
                const u64* handles = op->payload;
                for (u32 j = 0; j < count; j++) {
                    state->image[stage][first + j] = handles[j];
                    state->image_bound[stage] |= (u8)(1u << (first + j));
                }
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindProgram")) {
            replay_program_binds++;
            NvnProgramSnapshot program = {0};
            u32 mask = (u32)op->args[1];
            if (!mask) program_mask_zero++;
            if (mask & ~((1u << NVN_DRAW_SHADER_STAGES) - 1)) program_mask_invalid++;
            if (nvn_program_snapshot(op->args[0], &program)) {
                replay_program_resolved++;
                if (program.refs_known) {
                    replay_program_refs_known++;
                    replay_program_shader_refs += program.shader_count;
                    if (shader_transport && queue) {
                        for (u32 j = 0; j < program.shader_count; j++) {
                            u64 code = program.refs[j].code_candidate;
                            if (!code) continue;
                            int seen = 0;
                            for (u32 k = 0; k < queue->shader_count; k++) seen |= queue->shader_code[k] == code;
                            if (seen) continue;
                            if (queue->shader_count == 8)
                                shader_stage_full++;
                            else {
                                queue->shader_code[queue->shader_count++] = code;
                                shader_staged++;
                            }
                        }
                    }
                }
            } else if (nvn_program_check(op->args[0]) == -1) {
                replay_program_unconfigured++;
            } else {
                replay_program_missing++;
            }
            for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES; stage++) {
                if (!(mask & (1u << stage))) continue;
                state->program[stage] = (NvnStageProgram){.handle = op->args[0], .snapshot = program};
                state->program_bound |= 1u << stage;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindVertexBuffer") ||
                   !strcmp(op->name, "nvnInlinedVertexBind")) {
            u32 index = (u32)op->args[0];
            if (index >= NVN_DRAW_VERTEX_SLOTS)
                vertex_bind_invalid++;
            else if (!strcmp(op->name, "nvnInlinedVertexBind") && !op->args[3])
                state->vertex_bound &= (u16)~(1u << index);
            else {
                state->vertex[index] = (NvnBufferRange){op->args[1], op->args[2]};
                state->vertex_bound |= (u16)(1u << index);
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindVertexBuffers")) {
            u32 first = (u32)op->args[0], count = (u32)op->args[1];
            if (!op->payload || first >= NVN_DRAW_VERTEX_SLOTS || count > NVN_DRAW_VERTEX_SLOTS - first) {
                vertex_bind_invalid++;
                state->vertex_bound = 0;
            } else {
                const NvnBufferRange* ranges = op->payload;
                for (u32 j = 0; j < count; j++) {
                    state->vertex[first + j] = ranges[j];
                    state->vertex_bound |= (u16)(1u << (first + j));
                }
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindVertexAttribState")) {
            if (!op->payload) {
                state->vertex_attrib_count = 0;
            } else {
                const NvnVertexState* vertex = op->payload;
                state->vertex_attrib_count = vertex->count;
                memcpy(state->vertex_attrib_raw, vertex->attrib, sizeof vertex->attrib);
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindVertexStreamState")) {
            if (!op->payload) {
                state->vertex_stream_count = 0;
            } else {
                const NvnVertexState* vertex = op->payload;
                state->vertex_stream_count = vertex->count;
                memcpy(state->vertex_stream_raw, vertex->stream, sizeof vertex->stream);
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindChannelMaskState")) {
            state->channel_mask_known = !!op->payload;
            if (op->payload) state->channel_mask = *(const u32*)op->payload;
        } else if (!strcmp(op->name, "nvnCommandBufferBindColorState")) {
            state->color_state_known = !!op->payload;
            if (op->payload) state->color_state = *(const u32*)op->payload;
        } else if (!strcmp(op->name, "nvnCommandBufferBindPolygonState")) {
            state->polygon_state_known = !!op->payload;
            if (op->payload) state->polygon_state = *(const u32*)op->payload;
        } else if (!strcmp(op->name, "nvnCommandBufferBindDepthStencilState")) {
            state->depth_stencil_state_known = !!op->payload;
            if (op->payload) memcpy(state->depth_stencil_state, op->payload, sizeof state->depth_stencil_state);
        } else if (!strcmp(op->name, "nvnCommandBufferBindMultisampleState")) {
            state->multisample_state_known = !!op->payload;
            if (op->payload) memcpy(state->multisample_state, op->payload, sizeof state->multisample_state);
        } else if (!strcmp(op->name, "nvnCommandBufferBindBlendState")) {
            if (!op->payload) {
                state->blend_state_known = 0;
            } else {
                const u32* blend = op->payload;
                u32 target = blend[0] & 7;
                memcpy(state->blend_state[target], blend, sizeof state->blend_state[target]);
                state->blend_state_known |= 1u << target;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindUniformBuffer") ||
                   !strcmp(op->name, "nvnInlinedUniformBind")) {
            u32 stage = (u32)op->args[0], index = (u32)op->args[1];
            if (stage >= NVN_DRAW_SHADER_STAGES || index >= NVN_DRAW_UNIFORM_SLOTS)
                uniform_bind_invalid++;
            else if (!strcmp(op->name, "nvnInlinedUniformBind") && !op->args[4])
                state->uniform_bound[stage] &= (u16)~(1u << index);
            else {
                state->uniform[stage][index] = (NvnBufferRange){op->args[2], op->args[3]};
                state->uniform_bound[stage] |= (u16)(1u << index);
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindUniformBuffers")) {
            u32 stage = (u32)op->args[0], first = (u32)op->args[1], count = (u32)op->args[2];
            if (stage >= NVN_DRAW_SHADER_STAGES || first >= NVN_DRAW_UNIFORM_SLOTS ||
                count > NVN_DRAW_UNIFORM_SLOTS - first || !op->payload) {
                uniform_bind_invalid++;
                if (stage < NVN_DRAW_SHADER_STAGES) state->uniform_bound[stage] = 0;
            } else {
                const NvnBufferRange* ranges = op->payload;
                for (u32 j = 0; j < count; j++) {
                    state->uniform[stage][first + j] = ranges[j];
                    state->uniform_bound[stage] |= (u16)(1u << (first + j));
                }
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindStorageBuffer")) {
            u32 stage = (u32)op->args[0], index = (u32)op->args[1];
            const char* dispatch_trace = getenv("SNX_NVN_DISPATCH_TRACE");
            if (stage == 5 && dispatch_trace && !strcmp(dispatch_trace, "1"))
                LOG("nvn-compute-storage-bind: seq=%llu slot=%u addr=%llx size=%llu macro=%d word=%x\n",
                    (unsigned long long)(queue ? queue->next_sequence : 0), index,
                    (unsigned long long)op->args[2], (unsigned long long)op->args[3],
                    op->storage_cbuf_word_known, op->storage_cbuf_word);
            if (stage >= NVN_DRAW_SHADER_STAGES || index >= NVN_DRAW_STORAGE_SLOTS) {
                storage_bind_invalid++;
            } else {
                u16 bit = (u16)(1u << index);
                /* Compute bindings are semantic NVN ranges. Graphics bindings additionally need the
                   observed MME CBUF word before a draw can use them. */
                if ((stage == 5 || op->storage_cbuf_word_known) && op->args[2] && op->args[3]) {
                    state->storage_known[stage] &= (u16)~bit;
                    state->storage[stage][index] = (NvnStorageBinding){
                        .range = {op->args[2], op->args[3]}, .cbuf_word = op->storage_cbuf_word};
                    state->storage_known[stage] |= bit;
                } else if (stage == 5)
                    state->storage_known[stage] &= (u16)~bit;
                else
                    state->storage_known[stage] = 0;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferBindStorageBuffers")) {
            u32 stage = (u32)op->args[0], first = (u32)op->args[1], count = (u32)op->args[2];
            const char* dispatch_trace = getenv("SNX_NVN_DISPATCH_TRACE");
            if (stage == 5 && dispatch_trace && !strcmp(dispatch_trace, "1"))
                LOG("nvn-compute-storage-binds: seq=%llu first=%u count=%u\n",
                    (unsigned long long)(queue ? queue->next_sequence : 0), first, count);
            if (stage >= NVN_DRAW_SHADER_STAGES || first >= NVN_DRAW_STORAGE_SLOTS ||
                count > NVN_DRAW_STORAGE_SLOTS - first || (count && !op->payload)) {
                storage_bind_invalid++;
                if (stage < NVN_DRAW_SHADER_STAGES) state->storage_known[stage] = 0;
            } else if (stage == 5) {
                const NvnBufferRange* ranges = op->payload;
                for (u32 j = 0; j < count; j++) {
                    u16 bit = (u16)(1u << (first + j));
                    state->storage[stage][first + j].range = ranges[j];
                    if (ranges[j].address && ranges[j].size)
                        state->storage_known[stage] |= bit;
                    else
                        state->storage_known[stage] &= (u16)~bit;
                }
            } else {
                /* Graphics array macro offsets have not been paired with their semantic ranges. */
                state->storage_known[stage] = 0;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferDrawArrays") ||
                   !strcmp(op->name, "nvnCommandBufferDrawArraysInstanced") ||
                   !strcmp(op->name, "nvnCommandBufferDrawElements") ||
                   !strcmp(op->name, "nvnCommandBufferDrawElementsBaseVertex") ||
                   !strcmp(op->name, "nvnCommandBufferDrawElementsInstanced")) {
            NvnDrawCommand draw = {0};
            draw.last_unmodeled_write = queue ? queue->last_unmodeled_write : 0;
            draw.load_textures = texture_load;
            draw.prepare_attachments = attachment_prepare;
            draw.materialize_shaders = shader_materialize;
            draw.prepare_pipeline = pipeline_prepare;
            draw.program_bound = state->program_bound;
            memcpy(draw.program, state->program, sizeof draw.program);
            draw.program_known = !!state->program_bound;
            draw.shader_refs_known = !!state->program_bound;
            for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES; stage++) {
                if (!(state->program_bound & (1u << stage))) continue;
                draw.program_known &= !!draw.program[stage].snapshot.generation;
                draw.shader_refs_known &= !!draw.program[stage].snapshot.refs_known;
            }
            draw_vertex_program += !!draw.program[0].snapshot.generation;
            draw_fragment_program += !!draw.program[1].snapshot.generation;
            draw.primitive = (u32)op->args[0];
            draw.count = (u32)op->args[2];
            draw.instance_count = 1;
            if (!strcmp(op->name, "nvnCommandBufferDrawArraysInstanced"))
                draw.instance_count = (u32)op->args[4];
            else if (!strcmp(op->name, "nvnCommandBufferDrawElementsInstanced"))
                draw.instance_count = (u32)op->args[6];
            draw.vertex_bound = state->vertex_bound;
            memcpy(draw.vertex, state->vertex, sizeof draw.vertex);
            draw.vertex_attrib_count = state->vertex_attrib_count;
            draw.vertex_stream_count = state->vertex_stream_count;
            draw.channel_mask = state->channel_mask;
            draw.channel_mask_known = state->channel_mask_known;
            draw.color_state = state->color_state;
            draw.color_state_known = state->color_state_known;
            draw.polygon_state = state->polygon_state;
            draw.polygon_state_known = state->polygon_state_known;
            draw.depth_stencil_state_known = state->depth_stencil_state_known;
            memcpy(draw.depth_stencil_state, state->depth_stencil_state, sizeof draw.depth_stencil_state);
            draw.multisample_state_known = state->multisample_state_known;
            memcpy(draw.multisample_state, state->multisample_state, sizeof draw.multisample_state);
            draw.viewport_known = state->viewport_known;
            draw.scissor_known = state->scissor_known;
            draw.depth_range_known = state->depth_range_known;
            memcpy(draw.viewport, state->viewport, sizeof draw.viewport);
            memcpy(draw.scissor, state->scissor, sizeof draw.scissor);
            memcpy(draw.depth_range, state->depth_range, sizeof draw.depth_range);
            draw_viewport_known += !!(draw.viewport_known & 1);
            draw_scissor_known += !!(draw.scissor_known & 1);
            draw_depth_range_known += !!(draw.depth_range_known & 1);
            memcpy(draw.blend_color, state->blend_color, sizeof draw.blend_color);
            draw.blend_color_known = state->blend_color_known;
            memcpy(draw.stencil_value_mask, state->stencil_value_mask, sizeof draw.stencil_value_mask);
            memcpy(draw.stencil_write_mask, state->stencil_write_mask, sizeof draw.stencil_write_mask);
            memcpy(draw.stencil_ref, state->stencil_ref, sizeof draw.stencil_ref);
            draw.stencil_value_mask_known = state->stencil_value_mask_known;
            draw.stencil_write_mask_known = state->stencil_write_mask_known;
            draw.stencil_ref_known = state->stencil_ref_known;
            draw_blend_color_known += draw.blend_color_known;
            draw_stencil_value_mask_known += draw.stencil_value_mask_known == 3;
            draw_stencil_write_mask_known += draw.stencil_write_mask_known == 3;
            draw_stencil_ref_known += draw.stencil_ref_known == 3;
            draw.line_width = state->line_width;
            draw.line_width_known = state->line_width_known;
            draw.point_size = state->point_size;
            draw.point_size_known = state->point_size_known;
            draw.alpha_ref = state->alpha_ref;
            draw.alpha_ref_known = state->alpha_ref_known;
            memcpy(draw.polygon_offset, state->polygon_offset, sizeof draw.polygon_offset);
            draw.polygon_offset_known = state->polygon_offset_known;
            memcpy(draw.depth_bounds, state->depth_bounds, sizeof draw.depth_bounds);
            draw.depth_bounds_known = state->depth_bounds_known;
            draw.depth_bounds_enabled = state->depth_bounds_enabled;
            draw.sample_mask = state->sample_mask;
            draw.sample_mask_known = state->sample_mask_known;
            draw.rasterizer_discard = state->rasterizer_discard;
            draw.rasterizer_discard_known = state->rasterizer_discard_known;
            draw.depth_clamp = state->depth_clamp;
            draw.depth_clamp_known = state->depth_clamp_known;
            draw_line_width_known += draw.line_width_known;
            draw_polygon_offset_known += draw.polygon_offset_known;
            draw_depth_bounds_known += draw.depth_bounds_known;
            draw_sample_mask_known += draw.sample_mask_known;
            draw_rasterizer_discard_known += draw.rasterizer_discard_known;
            draw_depth_clamp_known += draw.depth_clamp_known;
            draw.blend_state_known = state->blend_state_known;
            memcpy(draw.blend_state, state->blend_state, sizeof draw.blend_state);
            memcpy(draw.vertex_attrib_raw, state->vertex_attrib_raw, sizeof draw.vertex_attrib_raw);
            memcpy(draw.vertex_stream_raw, state->vertex_stream_raw, sizeof draw.vertex_stream_raw);
            for (u32 j = 0; j < draw.vertex_attrib_count; j++) {
                u32 raw = draw.vertex_attrib_raw[j];
                NvnVertexAttrib* attrib = &draw.vertex_attrib[j];
                attrib->enabled = !!(raw & 0x40);
                attrib->stream = raw & 0x1f;
                attrib->offset = (raw >> 7) & 0x3fff;
                attrib->format = (raw >> 21) & 0x3ff;
                if (attrib->enabled) {
                    draw_enabled_attribs++;
                    if (draw.count && draw.instance_count &&
                        (attrib->stream >= draw.vertex_stream_count || !(draw.vertex_bound & (1u << attrib->stream)))) {
                        draw_bad_attrib_stream++;
                        WARN_LIMIT(12,
                                   "nvn-vertex-layout: missing buffer attr=%u stream=%u streams=%u bound=%04x "
                                   "format=%u offset=%u count=%u instances=%u program=%llx vertex-code=%llx\n",
                                   j, attrib->stream, draw.vertex_stream_count, draw.vertex_bound, attrib->format,
                                   attrib->offset, draw.count, draw.instance_count,
                                   (unsigned long long)draw.program[0].handle,
                                   (unsigned long long)draw.program[0].snapshot.refs[0].code_candidate);
                        if (shader_transport && queue) {
                            u64 code = draw.program[0].snapshot.refs[0].code_candidate;
                            int seen = 0;
                            for (u32 k = 0; k < queue->vertex_probe_count; k++)
                                seen |= queue->vertex_probe_code[k] == code;
                            if (code && !seen && queue->vertex_probe_count < 8)
                                queue->vertex_probe_code[queue->vertex_probe_count++] = code;
                        }
                    }
                }
            }
            for (u32 j = 0; j < draw.vertex_stream_count; j++) {
                draw.vertex_stream[j].stride = (u32)draw.vertex_stream_raw[j];
                draw.vertex_stream[j].divisor = (u32)(draw.vertex_stream_raw[j] >> 32);
            }
            memcpy(draw.uniform, state->uniform, sizeof draw.uniform);
            memcpy(draw.uniform_bound, state->uniform_bound, sizeof draw.uniform_bound);
            draw.uniform_updates = queue ? queue->uniform_updates : NULL;
            draw.uniform_updates_complete = queue && !queue->uniform_update_failed;
            if (draw_prepare && queue)
                for (u32 stage = 0; stage < 2; stage++)
                    for (u32 slot = 0; slot < NVN_DRAW_UNIFORM_SLOTS; slot++)
                        if (draw.uniform_bound[stage] & (1u << slot))
                            draw.uniform_seed[stage][slot] =
                                replay_storage_seed(queue, draw.uniform[stage][slot]);
            memcpy(draw.storage[0], state->storage[0], sizeof draw.storage[0]);
            memcpy(draw.storage[1], state->storage[1], sizeof draw.storage[1]);
            draw.storage_known[0] = state->storage_known[0];
            draw.storage_known[1] = state->storage_known[1];
            storage_draw_snapshots += !!(draw.storage_known[0] | draw.storage_known[1]);
            memcpy(draw.texture, state->texture, sizeof draw.texture);
            memcpy(draw.texture_bound, state->texture_bound, sizeof draw.texture_bound);
            memcpy(draw.image, state->image, sizeof draw.image);
            memcpy(draw.image_bound, state->image_bound, sizeof draw.image_bound);
            int image_complete = 1, image_targets_complete = 1, has_image = 0;
            for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES; stage++)
                for (u32 index = 0; index < NVN_DRAW_IMAGE_SLOTS; index++) {
                    if (!(draw.image_bound[stage] & (1u << index))) continue;
                    has_image = 1;
                    if (nvn_descriptor_snapshot(state->texture_pool, draw.image[stage][index],
                                                &draw.image_descriptor[stage][index]) == 1)
                        draw.image_descriptor_valid[stage] |= (u8)(1u << index);
                    else
                        image_complete = 0;
                    if (nvn_descriptor_target_snapshot(state->texture_pool, draw.image[stage][index],
                                                       &draw.image_target[stage][index]))
                        draw.image_target_valid[stage] |= (u8)(1u << index);
                    else
                        image_targets_complete = 0;
                }
            if (has_image) {
                draw_with_image++;
                if (image_complete)
                    draw_image_descriptors_complete++;
                else
                    draw_image_descriptors_incomplete++;
                if (image_targets_complete)
                    draw_image_targets_complete++;
                else
                    draw_image_targets_incomplete++;
            }
            int descriptors_complete = 1, tics_complete = 1, views_complete = 1, samplers_complete = 1,
                tscs_complete = 1, filter_wrap_complete = 1, has_texture = 0;
            for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES; stage++)
                for (u32 index = 0; index < NVN_DRAW_TEXTURE_SLOTS; index++) {
                    if (!(draw.texture_bound[stage] & (1u << index))) continue;
                    has_texture = 1;
                    if (nvn_descriptor_snapshot(state->texture_pool, draw.texture[stage][index],
                                                &draw.texture_descriptor[stage][index]) == 1) {
                        draw.texture_descriptor_valid[stage] |= 1u << index;
                        if (!draw.texture_descriptor[stage][index].view_known) views_complete = 0;
                        if (!draw.texture_descriptor[stage][index].tic_valid) tics_complete = 0;
                    } else {
                        descriptors_complete = 0;
                        tics_complete = 0;
                        views_complete = 0;
                    }
                    if (nvn_sampler_descriptor_snapshot(state->sampler_pool, draw.texture[stage][index],
                                                        &draw.sampler_descriptor[stage][index]) == 1) {
                        draw.sampler_descriptor_valid[stage] |= 1u << index;
                        if (!draw.sampler_descriptor[stage][index].tsc_valid) tscs_complete = 0;
                        if ((draw.sampler_descriptor[stage][index].state.known &
                             (NVN_SAMPLER_FILTER_KNOWN | NVN_SAMPLER_WRAP_KNOWN)) !=
                            (NVN_SAMPLER_FILTER_KNOWN | NVN_SAMPLER_WRAP_KNOWN))
                            filter_wrap_complete = 0;
                    } else {
                        samplers_complete = 0;
                        tscs_complete = 0;
                        filter_wrap_complete = 0;
                    }
                }
            if (has_texture) {
                if (descriptors_complete)
                    draw_texture_descriptors_complete++;
                else
                    draw_texture_descriptors_incomplete++;
                if (tics_complete)
                    draw_texture_tics_complete++;
                else
                    draw_texture_tics_incomplete++;
                if (views_complete)
                    draw_texture_views_complete++;
                else
                    draw_texture_views_incomplete++;
                if (samplers_complete)
                    draw_sampler_descriptors_complete++;
                else
                    draw_sampler_descriptors_incomplete++;
                if (tscs_complete)
                    draw_sampler_tscs_complete++;
                else
                    draw_sampler_tscs_incomplete++;
                if (filter_wrap_complete)
                    draw_sampler_filter_wrap_complete++;
                else
                    draw_sampler_filter_wrap_incomplete++;
            }
            draw.target_complete = !!state->targets.valid;
            for (u32 j = 0; j < state->targets.count && j < 8; j++) {
                if (!state->targets.color[j]) continue;
                if ((state->targets.color_views & (1u << j)) && !state->targets.color_view[j].valid) {
                    draw.target_complete = 0;
                    continue;
                }
                const NvnTextureViewSelection* view =
                    (state->targets.color_views & (1u << j)) ? &state->targets.color_view[j].selection : NULL;
                if (replay_target(state->targets.color[j], view, &state->targets.color_target[j],
                                  !!(state->targets.color_resolved & (1u << j)), &draw.color_target[j]) ==
                    NVN_TARGET_RESOLVED)
                    draw.color_target_mask |= 1u << j;
                else
                    draw.target_complete = 0;
            }
            if (state->targets.depth) {
                if (state->targets.depth_view && !state->targets.depth_view_data.valid) {
                    draw.target_complete = 0;
                } else {
                    const NvnTextureViewSelection* view =
                        state->targets.depth_view ? &state->targets.depth_view_data.selection : NULL;
                    if (replay_target(state->targets.depth, view, &state->targets.depth_target,
                                      state->targets.depth_resolved, &draw.depth_target) ==
                        NVN_TARGET_RESOLVED)
                        draw.depth_target_bound = 1;
                    else
                        draw.target_complete = 0;
                }
            }
            if (!draw.color_target_mask && !draw.depth_target_bound)
                draw_targets_none++;
            else if (draw.target_complete)
                draw_targets_complete++;
            else
                draw_targets_incomplete++;
            if (!strncmp(op->name, "nvnCommandBufferDrawElements", 28)) {
                draw_elements++;
                draw.indexed = 1;
                draw.index_type = (u32)op->args[1];
                draw.count = (u32)op->args[2];
                draw.index_address = op->args[3];
                if (!strcmp(op->name, "nvnCommandBufferDrawElementsBaseVertex") ||
                    !strcmp(op->name, "nvnCommandBufferDrawElementsInstanced")) {
                    draw.base_vertex = (i32)op->args[4];
                    draw_base_vertex++;
                }
                if (!strcmp(op->name, "nvnCommandBufferDrawElementsInstanced")) {
                    draw.base_instance = (u32)op->args[5];
                    draw.instance_count = (u32)op->args[6];
                    draw_instanced++;
                }
                if (draw.index_type > 2 || !draw.index_address) draw_index_invalid++;
            } else {
                draw_arrays++;
                draw.first = (u32)op->args[1];
                draw.count = (u32)op->args[2];
                if (!strcmp(op->name, "nvnCommandBufferDrawArraysInstanced")) {
                    draw.base_instance = (u32)op->args[3];
                    draw.instance_count = (u32)op->args[4];
                    draw_instanced++;
                }
            }
            draw_zero_count += !draw.count;
            draw_zero_instances += !draw.instance_count;
            draw_with_program += draw.program_known;
            draw_with_shader_refs += draw.shader_refs_known;
            if (draw.vertex_bound)
                draw_with_vertex++;
            else
                draw_without_vertex++;
            draw_with_attrib_state += !!draw.vertex_attrib_count;
            draw_with_stream_state += !!draw.vertex_stream_count;
            u16 any_uniform = 0;
            for (u32 j = 0; j < NVN_DRAW_SHADER_STAGES; j++) any_uniform |= draw.uniform_bound[j];
            if (any_uniform)
                draw_with_uniform++;
            else
                draw_without_uniform++;
            u32 any_texture = 0;
            for (u32 j = 0; j < NVN_DRAW_SHADER_STAGES; j++) any_texture |= draw.texture_bound[j];
            if (any_texture)
                draw_with_texture++;
            else
                draw_without_texture++;
            static int initial_target_trace;
            const char* zero_probe = getenv("SNX_NVN_SHADOW_ZERO_PROBE");
            if (!initial_target_trace && (getenv("SNX_NVN_SHADOW_INPUT_TRACE") ||
                                          (zero_probe && !strcmp(zero_probe, "1"))) &&
                (draw.color_target_mask & 1)) {
                const NvnTextureTarget* target = &draw.color_target[0];
                const u8* host = target->storage_size && target->storage_size <= (16u << 20)
                                     ? gmem_ptr_untracked(target->gpu_address, target->storage_size) : NULL;
                if (host) {
                    u64 nonzero = 0;
                    for (u32 byte = 0; byte < target->storage_size; byte++) nonzero += !!host[byte];
                    LOG("nvn-shadow-target-input: addr=%llx size=%u nonzero=%llu draw=%u indexed=%d sequence=%llu\n",
                        (unsigned long long)target->gpu_address, target->storage_size,
                        (unsigned long long)nonzero, draw.count, draw.indexed,
                        (unsigned long long)(queue ? queue->next_sequence : 0));
                    if (zero_probe && !strcmp(zero_probe, "1") && !nonzero)
                        draw.color_initial_zero = 1;
                    initial_target_trace = 1;
                }
            }
            if (queue) {
                NvnDrawBatch* batch = queue->draws;
                if (!batch || batch->count == batch->capacity) {
                    u32 capacity = batch ? batch->capacity * 2 : 8;
                    if (capacity > 4096) capacity = 4096;
                    if (!batch || batch->capacity < capacity) {
                        NvnDrawBatch* grown = realloc(batch, sizeof *batch + (size_t)capacity * sizeof draw);
                        if (grown) {
                            if (!batch) grown->count = 0;
                            grown->capacity = capacity;
                            queue->draws = batch = grown;
                        }
                    }
                }
                if (batch && batch->count < batch->capacity) {
                    draw.sequence = queue->next_sequence++;
                    batch->commands[batch->count++] = draw;
                    draw_staged++;
                } else {
                    draw_stage_full++;
                }
                queue->last_unmodeled_write = queue->next_sequence++;
            }
        } else if (op->kind == OP_DRAW) {
            draw_other++;
            if (queue) queue->last_unmodeled_write = queue->next_sequence++;
        } else if (op->kind == OP_DISPATCH) {
            const char* trace = getenv("SNX_NVN_DISPATCH_TRACE");
            if (!dispatch_execute && (!trace || strcmp(trace, "1"))) continue;
            NvnDispatchCommand dispatch = {.sequence = queue ? queue->last_unmodeled_write : 0,
                                           .prior_unmodeled_write = prior_unmodeled_write,
                                           .program = state->program[5],
                                           .uniform_bound = state->uniform_bound[5],
                                           .storage_bound = state->storage_known[5]};
            for (u32 axis = 0; axis < 3; axis++) dispatch.groups[axis] = (u32)op->args[axis];
            memcpy(dispatch.uniform, state->uniform[5], sizeof dispatch.uniform);
            dispatch.uniform_updates = queue ? queue->uniform_updates : NULL;
            dispatch.uniform_updates_complete = queue && !queue->uniform_update_failed;
            if (dispatch_execute && queue)
                for (u32 slot = 0; slot < 5; slot++) {
                    const NvnBufferRange* range = &dispatch.uniform[slot];
                    if (!(dispatch.uniform_bound & (1u << slot)) || !range->address ||
                        !range->size || range->size > 0x10000) continue;
                    u64 padded = (range->size + 255) & ~255ull;
                    if (padded > 0x10000) padded = 0x10000;
                    dispatch.uniform_seed[slot] =
                        replay_storage_seed(queue, (NvnBufferRange){range->address, padded});
                }
            if (dispatch_execute && (dispatch.uniform_bound & 1) &&
                dispatch.uniform[0].size && dispatch.uniform[0].size <= sizeof dispatch.uniform0_snapshot) {
                u64 padded = (dispatch.uniform[0].size + 255) & ~255ull;
                if (padded <= sizeof dispatch.uniform0_snapshot) {
                    const u8* host = gmem_ptr_untracked(dispatch.uniform[0].address, padded);
                    if (host) {
                        memcpy(dispatch.uniform0_snapshot, host, padded);
                        dispatch.uniform0_snapshot_size = (u16)padded;
                    }
                }
            }
            for (u32 slot = 0; slot < NVN_DRAW_STORAGE_SLOTS; slot++) {
                dispatch.storage[slot] = state->storage[5][slot].range;
                if (dispatch_execute && queue && (dispatch.storage_bound & (1u << slot)))
                    dispatch.storage_seed[slot] = replay_storage_seed(queue, dispatch.storage[slot]);
            }
            const char* input_trace = getenv("SNX_NVN_SHADOW_INPUT_TRACE");
            if (input_trace && !strcmp(input_trace, "1")) {
                static u32 storage_probes;
                for (u32 slot = 0; slot < NVN_DRAW_STORAGE_SLOTS && storage_probes < 4; slot++) {
                    const NvnBufferRange* range = &dispatch.storage[slot];
                    if (!(dispatch.storage_bound & (1u << slot)) || range->size != 58848) continue;
                    const u8* host = gmem_ptr_untracked(range->address, range->size);
                    if (!host) continue;
                    u64 hash = 1469598103934665603ull;
                    for (u64 byte = 0; byte < range->size; byte++)
                        hash = (hash ^ host[byte]) * 1099511628211ull;
                    LOG("nvn-dispatch-storage-snapshot: addr=%llx seq=%llu hash=%llx byte304=%02x\n",
                        (unsigned long long)range->address, (unsigned long long)dispatch.sequence,
                        (unsigned long long)hash, host[304]);
                    storage_probes++;
                }
            }
            memcpy(dispatch.texture, state->texture[5], sizeof dispatch.texture);
            dispatch.texture_bound = state->texture_bound[5];
            if (dispatch_execute && (dispatch.texture_bound & 1)) {
                dispatch.texture0_descriptor_valid =
                    nvn_descriptor_snapshot(state->texture_pool, dispatch.texture[0],
                                            &dispatch.texture0_descriptor) == 1;
                dispatch.texture0_sampler_valid =
                    nvn_sampler_descriptor_snapshot(state->sampler_pool, dispatch.texture[0],
                                                    &dispatch.texture0_sampler) == 1;
            }
            if (dispatch.program.snapshot.refs_known && dispatch.program.snapshot.shader_count) {
                u64 control = dispatch.program.snapshot.refs[0].control_candidate;
                if (readable_range(control, 0x758)) {
                    const u32* words = (const u32*)GP(control);
                    if (words[0] == 0x98761234 && words[0x20 / 4] == 0x880) {
                        memcpy(dispatch.local, words + 0x748 / 4, sizeof dispatch.local);
                        dispatch.shared_size = words[0x754 / 4];
                        dispatch.embedded_cbuf1_offset = words[0x700 / 4];
                        dispatch.control_known = 1;
                    }
                }
            }
            if (queue && dispatch_execute) {
                NvnDispatchBatch* batch = queue->dispatches;
                if (!batch || batch->count == batch->capacity) {
                    u32 capacity = batch ? batch->capacity * 2 : 8;
                    if (capacity > 4096) capacity = 4096;
                    if (!batch || batch->capacity < capacity) {
                        NvnDispatchBatch* grown =
                            realloc(batch, sizeof *batch + (size_t)capacity * sizeof dispatch);
                        if (grown) {
                            if (!batch) grown->count = 0;
                            grown->capacity = capacity;
                            queue->dispatches = batch = grown;
                        }
                    }
                }
                if (batch && batch->count < batch->capacity)
                    batch->commands[batch->count++] = dispatch;
                else
                    allocation_failures++;
            }
            if (trace && !strcmp(trace, "1")) {
                const NvnStageProgram* program = &state->program[5];
                LOG("nvn-dispatch-textures: code=%llx bound=%08x handles=%llx,%llx,%llx,%llx\n",
                    (unsigned long long)program->snapshot.refs[0].code_candidate,
                    state->texture_bound[5],
                    (unsigned long long)state->texture[5][0],
                    (unsigned long long)state->texture[5][1],
                    (unsigned long long)state->texture[5][2],
                    (unsigned long long)state->texture[5][3]);
                LOG("nvn-dispatch-replay: seq=%llu name=%s args=%llx,%llx,%llx,%llx,%llx program=%llx code=%llx refs=%d uniforms=%04x storage=%04x local=%ux%ux%u shared=%u known=%d u0=%llx/%llu u1=%llx/%llu u2=%llx/%llu u3=%llx/%llu\n",
                    (unsigned long long)(queue ? queue->last_unmodeled_write : 0), op->name,
                    (unsigned long long)op->args[0], (unsigned long long)op->args[1],
                    (unsigned long long)op->args[2], (unsigned long long)op->args[3],
                    (unsigned long long)op->args[4], (unsigned long long)program->handle,
                    (unsigned long long)program->snapshot.refs[0].code_candidate,
                    program->snapshot.refs_known, state->uniform_bound[5], state->storage_known[5],
                    dispatch.local[0], dispatch.local[1], dispatch.local[2], dispatch.shared_size,
                    dispatch.control_known,
                    (unsigned long long)state->uniform[5][0].address,
                    (unsigned long long)state->uniform[5][0].size,
                    (unsigned long long)state->uniform[5][1].address,
                    (unsigned long long)state->uniform[5][1].size,
                    (unsigned long long)state->uniform[5][2].address,
                    (unsigned long long)state->uniform[5][2].size,
                    (unsigned long long)state->uniform[5][3].address,
                    (unsigned long long)state->uniform[5][3].size);
            }
        } else if (!strcmp(op->name, "nvnCommandBufferCallCommands")) {
            if (op->refs_truncated) {
                replay_truncated_refs++;
                if (queue) {
                    queue->observed_clear_only = 0;
                    queue->last_unmodeled_write = queue->next_sequence++;
                }
            }
            const u64* refs = op->ref_count > 8 ? op->payload : op->refs;
            for (u32 j = 0; j < op->ref_count; j++) replay_list(refs[j], state, queue, depth + 1);
        }
    }
}

static void reset(NvnList* r) {
    for (u32 i = 0; i < r->count; i++) free(r->ops[i].payload);
    free(r->ops);
    memset(r, 0, sizeof *r);
}

static int readable_range(u64 addr, u64 size) {
    if (!size || addr < AS_BASE || addr >= AS_SIZE || size > AS_SIZE - addr) return 0;
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

static int inlined_indexed_draw(u64 address, u64 size, u64 args[7]) {
    if (size < 24 || size > 65536 || (size & 3) || !readable_range(address, size)) return 0;
    NvnGapIndexedDraw draw;
    if (!nvn_command_gap_indexed_draw((const u32*)GP(address), (u32)(size / 4), &draw)) return 0;
    args[0] = draw.primitive;
    args[1] = draw.index_type;
    args[2] = draw.count;
    args[3] = draw.index_address;
    return 1;
}

static int append_inlined_op(NvnList* list, u8 kind, const char* name, const u64 args[7]) {
    if (list->count == MAX_OPS) {
        list->truncated = 1;
        dropped_ops++;
        return 0;
    }
    if (list->count == list->capacity) {
        u16 capacity = list->capacity ? list->capacity * 2 : 16;
        NvnOp* grown = realloc(list->ops, (size_t)capacity * sizeof *grown);
        if (!grown) {
            allocation_failures++;
            list->truncated = 1;
            dropped_ops++;
            return 0;
        }
        list->ops = grown;
        list->capacity = capacity;
    }
    NvnOp* op = &list->ops[list->count++];
    memset(op, 0, sizeof *op);
    op->kind = kind;
    op->name = name;
    memcpy(op->args, args, sizeof op->args);
    return 1;
}

static void append_inlined_draw(NvnList* list, const u64 args[7]) {
    inlined_draws += append_inlined_op(list, OP_DRAW, "nvnCommandBufferDrawElements", args);
}

static void capture_gap_method(u32 subchannel, u32 method, u32 value, void* context) {
    NvnList* list = context;
    if (subchannel) return;
    if (method >= 0x700 && method < 0x740) {
        u32 stream = (method - 0x700) / 4;
        switch ((method - 0x700) & 3) {
            case 1: list->gap_vertex[stream].start_high = value; list->gap_vertex[stream].known |= 1; break;
            case 2: list->gap_vertex[stream].start_low = value; list->gap_vertex[stream].known |= 2; break;
            default: return;
        }
        list->gap_vertex_dirty |= (u16)(1u << stream);
        return;
    }
    if (method >= 0x7c0 && method < 0x7e0) {
        u32 stream = (method - 0x7c0) / 2;
        if ((method - 0x7c0) & 1) {
            list->gap_vertex[stream].end_low = value;
            list->gap_vertex[stream].known |= 8;
        } else {
            list->gap_vertex[stream].end_high = value;
            list->gap_vertex[stream].known |= 4;
        }
        list->gap_vertex_dirty |= (u16)(1u << stream);
        return;
    }
    switch (method) {
        case 0x8e0: list->cb_size = value; list->cb_known |= 1; return;
        case 0x8e1: list->cb_high = value; list->cb_known |= 2; return;
        case 0x8e2: list->cb_low = value; list->cb_known |= 4; return;
        default: break;
    }
    if (method != 0x904 && method != 0x924) return;
    u32 bank = (value >> 4) & 31;
    if (bank < 3 || bank - 3 >= NVN_DRAW_UNIFORM_SLOTS) return;
    u32 stage = method == 0x904 ? 0 : 1;
    u64 args[7] = {stage, bank - 3, 0, 0, !!(value & 1)};
    if (value & 1) {
        if (list->cb_known != 7 || !list->cb_size || list->cb_size > 65536 || list->cb_high >= 256) {
            inlined_uniform_unknown++;
            return;
        }
        args[2] = ((u64)list->cb_high << 32) | list->cb_low;
        args[3] = list->cb_size;
    }
    inlined_uniform_binds += append_inlined_op(list, OP_STATE, "nvnInlinedUniformBind", args);
}

static void flush_gap_vertex_binds(NvnList* list) {
    u16 dirty = list->gap_vertex_dirty;
    list->gap_vertex_dirty = 0;
    for (u32 stream = 0; stream < NVN_DRAW_VERTEX_SLOTS; stream++) {
        if (!(dirty & (1u << stream))) continue;
        if (list->gap_vertex[stream].known != 15 || list->gap_vertex[stream].start_high >= 256 ||
            list->gap_vertex[stream].end_high >= 256) {
            inlined_vertex_unknown++;
            continue;
        }
        u64 start = ((u64)list->gap_vertex[stream].start_high << 32) | list->gap_vertex[stream].start_low;
        u64 end = ((u64)list->gap_vertex[stream].end_high << 32) | list->gap_vertex[stream].end_low;
        u64 size = end >= start ? end - start + 1 : 0;
        u64 args[7] = {stream, start, size, !!start && size <= (64u << 20)};
        inlined_vertex_binds += append_inlined_op(list, OP_STATE, "nvnInlinedVertexBind", args);
    }
}

/* Fingerprint the observed SDK's default constructor, which writes the entire view object. */
void nvn_record_set_view_layout(u32 first_instruction, u32 second_instruction, u32 third_instruction) {
    nvn_texture_set_view_layout(first_instruction, second_instruction, third_instruction);
}

static void snapshot_view(u64 ptr, NvnView* view) {
    if (!nvn_texture_view_snapshot(ptr, &view->selection)) {
        view_snapshot_failures++;
        return;
    }
    for (u32 i = 0; i < 6; i++) view_flags[i] += !!(view->selection.flags & (1u << i));
    view->valid = 1;
    view_snapshots++;
}

static int snapshot_target(u64 texture, const NvnTextureViewSelection* view, NvnTextureTarget* out) {
    NvnTargetResult result = nvn_texture_resolve_target(texture, view, out);
    if (result == NVN_TARGET_MISSING)
        result = nvn_texture_resolve_copied_target(texture, view, out);
    return result == NVN_TARGET_RESOLVED;
}

/* The SDK consumes these pointers immediately. Copy them before the caller can reuse its stack. */
static void snapshot_semantic(NvnOp* op, const CPU* c) {
    if (!strcmp(op->name, "nvnCommandBufferUpdateUniformBuffer")) {
        u64 address = op->args[0], buffer_size = op->args[1], offset = op->args[2];
        u64 size = op->args[3], source = op->args[4];
        if (!size || size > 65536 || offset > buffer_size || size > buffer_size - offset ||
            address > UINT64_MAX - offset || !readable_range(source, size)) return;
        void* bytes = malloc((size_t)size);
        if (!bytes) { allocation_failures++; return; }
        memcpy(bytes, GP(source), (size_t)size);
        op->payload = bytes;
    } else if (!strcmp(op->name, "nvnCommandBufferSetBlendColor")) {
        if (!readable_range(op->args[0], 4 * sizeof(u32))) {
            invalid_arrays++;
            return;
        }
        u32* color = malloc(4 * sizeof *color);
        if (!color) {
            allocation_failures++;
            return;
        }
        for (u32 j = 0; j < 4; j++) color[j] = gr32(op->args[0] + (u64)j * 4);
        op->payload = color;
    } else if (!strcmp(op->name, "nvnCommandBufferSetLineWidth") || !strcmp(op->name, "nvnCommandBufferSetPointSize") ||
               !strcmp(op->name, "nvnCommandBufferSetAlphaRef") ||
               !strcmp(op->name, "nvnCommandBufferSetPolygonOffsetClamp") ||
               !strcmp(op->name, "nvnCommandBufferSetDepthBounds")) {
        u32 count = !strcmp(op->name, "nvnCommandBufferSetPolygonOffsetClamp") ? 3
                    : !strcmp(op->name, "nvnCommandBufferSetDepthBounds")      ? 2
                                                                               : 1;
        u32* values = malloc((size_t)count * sizeof *values);
        if (!values) {
            allocation_failures++;
            return;
        }
        for (u32 j = 0; j < count; j++) values[j] = c->v[j].s[0];
        op->payload = values;
    } else if (!strcmp(op->name, "nvnCommandBufferSetViewports") || !strcmp(op->name, "nvnCommandBufferSetScissors") ||
               !strcmp(op->name, "nvnCommandBufferSetDepthRanges") ||
               !strcmp(op->name, "nvnCommandBufferSetDepthRange")) {
        int scalar_depth = !strcmp(op->name, "nvnCommandBufferSetDepthRange");
        u32 first = scalar_depth ? 0 : (u32)op->args[0];
        u32 count = scalar_depth ? 1 : (u32)op->args[1];
        u32 words =
            !strcmp(op->name, "nvnCommandBufferSetViewports") || !strcmp(op->name, "nvnCommandBufferSetScissors") ? 4
                                                                                                                  : 2;
        if (first > NVN_DRAW_VIEWPORT_SLOTS || count > NVN_DRAW_VIEWPORT_SLOTS - first ||
            (!scalar_depth && count && !readable_range(op->args[2], (u64)count * words * sizeof(u32)))) {
            invalid_arrays++;
            return;
        }
        NvnDynamicArray* array = malloc(sizeof *array + (size_t)count * words * sizeof(u32));
        if (!array) {
            allocation_failures++;
            return;
        }
        array->first = (u8)first;
        array->count = (u8)count;
        array->words = (u8)words;
        if (scalar_depth) {
            array->values[0] = c->v[0].s[0];
            array->values[1] = c->v[1].s[0];
        } else
            for (u32 j = 0; j < count * words; j++) array->values[j] = gr32(op->args[2] + (u64)j * 4);
        op->payload = array;
    } else if (!strcmp(op->name, "nvnCommandBufferBindChannelMaskState") ||
               !strcmp(op->name, "nvnCommandBufferBindColorState") ||
               !strcmp(op->name, "nvnCommandBufferBindPolygonState") ||
               !strcmp(op->name, "nvnCommandBufferBindBlendState") ||
               !strcmp(op->name, "nvnCommandBufferBindDepthStencilState") ||
               !strcmp(op->name, "nvnCommandBufferBindMultisampleState")) {
        u32 count = !strcmp(op->name, "nvnCommandBufferBindMultisampleState") ? 6
                    : !strcmp(op->name, "nvnCommandBufferBindBlendState") ||
                            !strcmp(op->name, "nvnCommandBufferBindDepthStencilState")
                        ? 2
                        : 1;
        if (!readable_range(op->args[0], (u64)count * sizeof(u32))) return;
        u32* words = malloc(count * sizeof *words);
        if (!words) {
            allocation_failures++;
            return;
        }
        for (u32 i = 0; i < count; i++) words[i] = gr32(op->args[0] + (u64)i * 4);
        op->payload = words;
    } else if (!strcmp(op->name, "nvnCommandBufferCopyTextureToTexture")) {
        NvnView source_view = {0}, target_view = {0};
        if (op->args[1]) snapshot_view(op->args[1], &source_view);
        if (op->args[4]) snapshot_view(op->args[4], &target_view);
        if (op->args[4] && !target_view.valid) return;
        NvnTextureCopy* captured = calloc(1, sizeof *captured);
        if (!captured) {
            allocation_failures++;
            return;
        }
        const NvnTextureViewSelection* target_selection = op->args[4] ? &target_view.selection : NULL;
        if (!snapshot_target(op->args[3], target_selection, &captured->target) ||
            !captured->target.storage_size) {
            free(captured);
            return;
        }
        if (!op->args[1] || source_view.valid) {
            const NvnTextureViewSelection* source_selection = op->args[1] ? &source_view.selection : NULL;
            captured->source_valid = snapshot_target(op->args[0], source_selection, &captured->source) &&
                                     !!captured->source.storage_size;
        }
        if (readable_range(op->args[2], sizeof captured->source_region) &&
            readable_range(op->args[5], sizeof captured->target_region)) {
            for (u32 i = 0; i < 6; i++) {
                captured->source_region[i] = (i32)gr32(op->args[2] + (u64)i * 4);
                captured->target_region[i] = (i32)gr32(op->args[5] + (u64)i * 4);
            }
            captured->regions_valid = 1;
        }
        op->payload = captured;
    } else if (!strcmp(op->name, "nvnCommandBufferCopyBufferToTexture")) {
        if (!readable_range(op->args[3], 24)) {
            copy_snapshot_failures++;
            return;
        }
        NvnCopyRegion* copy = calloc(1, sizeof *copy);
        if (!copy) {
            allocation_failures++;
            copy_snapshot_failures++;
            return;
        }
        for (u32 i = 0; i < 6; i++) copy->region[i] = (i32)gr32(op->args[3] + (u64)i * 4);
        copy->has_view = !!op->args[2];
        if (copy->has_view) snapshot_view(op->args[2], &copy->view);
        copy->valid = !copy->has_view || copy->view.valid;
        op->payload = copy;
        copy_snapshots++;
    } else if (!strcmp(op->name, "nvnCommandBufferClearTexture")) {
        if (!readable_range(op->args[2], 24) || !readable_range(op->args[3], 16)) {
            clear_snapshot_failures++;
            return;
        }
        NvnClearCommand* clear = calloc(1, sizeof *clear);
        if (!clear) {
            allocation_failures++;
            clear_snapshot_failures++;
            return;
        }
        NvnView view = {0};
        if (op->args[1]) snapshot_view(op->args[1], &view);
        if (!op->args[1] || view.valid) {
            NvnTextureTarget target = {0};
            if (snapshot_target(op->args[0], op->args[1] ? &view.selection : NULL, &target))
                clear->target = target;
        }
        clear->kind = NVN_CLEAR_COLOR;
        clear->mask = (u32)op->args[4];
        clear->region_known = 1;
        for (u32 i = 0; i < 6; i++) clear->region[i] = (i32)gr32(op->args[2] + (u64)i * 4);
        for (u32 i = 0; i < 4; i++) clear->color[i] = gr32(op->args[3] + (u64)i * 4);
        op->payload = clear;
        clear_snapshots++;
    } else if (!strcmp(op->name, "nvnCommandBufferClearColor")) {
        if (!readable_range(op->args[1], 16)) {
            clear_snapshot_failures++;
            return;
        }
        NvnClearColor* clear = malloc(sizeof *clear);
        if (!clear) {
            allocation_failures++;
            clear_snapshot_failures++;
            return;
        }
        for (u32 i = 0; i < 4; i++) clear->color[i] = gr32(op->args[1] + (u64)i * 4);
        clear->index = (u32)op->args[0];
        clear->mask = (u32)op->args[2];
        op->payload = clear;
        clear_snapshots++;
    } else if (!strcmp(op->name, "nvnCommandBufferClearDepthStencil")) {
        NvnClearDepth* clear = malloc(sizeof *clear);
        if (!clear) {
            allocation_failures++;
            clear_snapshot_failures++;
            return;
        }
        clear->depth = c->v[0].s[0];
        clear->depth_mask = (u32)op->args[0];
        clear->stencil = (u32)op->args[1];
        clear->stencil_mask = (u32)op->args[2];
        op->payload = clear;
        depth_snapshots++;
    } else if (!strcmp(op->name, "nvnCommandBufferSetRenderTargets")) {
        u32 count = (u32)op->args[0];
        u64 colors = op->args[1], views = op->args[2];
        if (count > 8 || (count && !readable_range(colors, (u64)count * 8)) ||
            (count && views && !readable_range(views, (u64)count * 8))) {
            target_snapshot_failures++;
            return;
        }
        NvnTargets* targets = calloc(1, sizeof *targets);
        if (!targets) {
            allocation_failures++;
            target_snapshot_failures++;
            return;
        }
        targets->count = count;
        targets->valid = 1;
        for (u32 i = 0; i < count; i++) {
            targets->color[i] = gr64(colors + (u64)i * 8);
            if (views) {
                u64 view = gr64(views + (u64)i * 8);
                if (view) {
                    targets->color_views |= 1u << i;
                    snapshot_view(view, &targets->color_view[i]);
                }
            }
            if (targets->color[i] && (!(targets->color_views & (1u << i)) || targets->color_view[i].valid)) {
                const NvnTextureViewSelection* view =
                    (targets->color_views & (1u << i)) ? &targets->color_view[i].selection : NULL;
                if (snapshot_target(targets->color[i], view, &targets->color_target[i]))
                    targets->color_resolved |= 1u << i;
            }
        }
        targets->depth = op->args[3];
        targets->depth_view = !!op->args[4];
        if (targets->depth_view) snapshot_view(op->args[4], &targets->depth_view_data);
        if (targets->depth && (!targets->depth_view || targets->depth_view_data.valid)) {
            const NvnTextureViewSelection* view =
                targets->depth_view ? &targets->depth_view_data.selection : NULL;
            targets->depth_resolved = snapshot_target(targets->depth, view, &targets->depth_target);
        }
        op->payload = targets;
        target_snapshots++;
    } else if (!strcmp(op->name, "nvnCommandBufferBindVertexBuffers")) {
        u32 first = (u32)op->args[0], count = (u32)op->args[1];
        if (first >= NVN_DRAW_VERTEX_SLOTS || count > NVN_DRAW_VERTEX_SLOTS - first ||
            (count && !readable_range(op->args[2], (u64)count * sizeof(NvnBufferRange)))) {
            vertex_range_snapshot_failures++;
            return;
        }
        NvnBufferRange* ranges = calloc(count ? count : 1, sizeof *ranges);
        if (!ranges) {
            allocation_failures++;
            vertex_range_snapshot_failures++;
            return;
        }
        for (u32 i = 0; i < count; i++) {
            ranges[i].address = gr64(op->args[2] + (u64)i * sizeof *ranges);
            ranges[i].size = gr64(op->args[2] + (u64)i * sizeof *ranges + 8);
        }
        op->payload = ranges;
        vertex_range_snapshots++;
    } else if (!strcmp(op->name, "nvnCommandBufferBindVertexAttribState") ||
               !strcmp(op->name, "nvnCommandBufferBindVertexStreamState")) {
        int attrib = !strcmp(op->name, "nvnCommandBufferBindVertexAttribState");
        u32 count = (u32)op->args[0];
        u32 element_size = attrib ? 4 : 8;
        if (count > NVN_DRAW_VERTEX_SLOTS || (count && !readable_range(op->args[1], (u64)count * element_size))) {
            vertex_state_snapshot_failures++;
            return;
        }
        NvnVertexState* vertex = calloc(1, sizeof *vertex);
        if (!vertex) {
            allocation_failures++;
            vertex_state_snapshot_failures++;
            return;
        }
        vertex->count = count;
        for (u32 i = 0; i < count; i++) {
            if (attrib)
                vertex->attrib[i] = gr32(op->args[1] + (u64)i * 4);
            else
                vertex->stream[i] = gr64(op->args[1] + (u64)i * 8);
        }
        op->payload = vertex;
        if (attrib)
            vertex_attrib_snapshots++;
        else
            vertex_stream_snapshots++;
    } else if (!strcmp(op->name, "nvnCommandBufferBindUniformBuffers") ||
               !strcmp(op->name, "nvnCommandBufferBindStorageBuffers")) {
        int storage = !strcmp(op->name, "nvnCommandBufferBindStorageBuffers");
        u32 stage = (u32)op->args[0], first = (u32)op->args[1], count = (u32)op->args[2];
        u32 limit = storage ? NVN_DRAW_STORAGE_SLOTS : NVN_DRAW_UNIFORM_SLOTS;
        if (stage >= NVN_DRAW_SHADER_STAGES || first >= limit ||
            count > limit - first ||
            (count && !readable_range(op->args[3], (u64)count * sizeof(NvnBufferRange)))) {
            uniform_range_snapshot_failures++;
            return;
        }
        NvnBufferRange* ranges = calloc(count ? count : 1, sizeof *ranges);
        if (!ranges) {
            allocation_failures++;
            uniform_range_snapshot_failures++;
            return;
        }
        for (u32 i = 0; i < count; i++) {
            ranges[i].address = gr64(op->args[3] + (u64)i * sizeof *ranges);
            ranges[i].size = gr64(op->args[3] + (u64)i * sizeof *ranges + 8);
        }
        op->payload = ranges;
        uniform_range_snapshots++;
    } else if (!strcmp(op->name, "nvnCommandBufferBindTextures") ||
               !strcmp(op->name, "nvnCommandBufferBindImages")) {
        int images = !strcmp(op->name, "nvnCommandBufferBindImages");
        u32 limit = images ? NVN_DRAW_IMAGE_SLOTS : NVN_DRAW_TEXTURE_SLOTS;
        u32 stage = (u32)op->args[0], first = (u32)op->args[1], count = (u32)op->args[2];
        if (stage >= NVN_DRAW_SHADER_STAGES || first >= limit || count > limit - first ||
            (count && !readable_range(op->args[3], (u64)count * 8))) {
            if (images)
                image_handle_snapshot_failures++;
            else
                texture_handle_snapshot_failures++;
            return;
        }
        u64* handles = calloc(count ? count : 1, sizeof *handles);
        if (!handles) {
            allocation_failures++;
            if (images)
                image_handle_snapshot_failures++;
            else
                texture_handle_snapshot_failures++;
            return;
        }
        for (u32 i = 0; i < count; i++) handles[i] = gr64(op->args[3] + (u64)i * 8);
        op->payload = handles;
        if (images)
            image_handle_snapshots++;
        else
            texture_handle_snapshots++;
    }
}

static int kind(const char* name) {
    const char* op = name + strlen("nvnCommandBuffer");
    if (!strcmp(op, "AddCommandMemory") || !strcmp(op, "AddControlMemory")) return OP_MEMORY;
    if (!strncmp(op, "Draw", 4)) return OP_DRAW;
    if (!strncmp(op, "Clear", 5)) return OP_CLEAR;
    if (!strncmp(op, "Dispatch", 8)) return OP_DISPATCH;
    if (!strncmp(op, "Call", 4)) return OP_CALL;
    if (!strncmp(op, "Bind", 4) || !strncmp(op, "Set", 3)) return OP_STATE;
    if (!strncmp(op, "Copy", 4) || !strncmp(op, "Update", 6) || !strncmp(op, "Resolve", 7)) return OP_TRANSFER;
    if (!strncmp(op, "Barrier", 7) || !strncmp(op, "Invalidate", 10)) return OP_BARRIER;
    if (!strncmp(op, "Fence", 5) || !strncmp(op, "Wait", 4)) return OP_SYNC;
    return OP_OTHER;
}

int nvn_record_wants(const char* name) {
    if (!strcmp(name, "nvnQueueSubmitCommands") || !strcmp(name, "nvnCommandBufferBeginRecording") ||
        !strcmp(name, "nvnCommandBufferEndRecording") || !strcmp(name, "nvnQueueFlush") ||
        !strcmp(name, "nvnQueuePresentTexture"))
        return 1;
    if (strncmp(name, "nvnCommandBuffer", 16)) return 0;
    if (!strcmp(name, "nvnCommandBufferSetMemoryCallback") || !strcmp(name, "nvnCommandBufferSetMemoryCallbackData"))
        return 0;
    return kind(name) != OP_OTHER;
}

u64 nvn_record_before(const CPU* c, const char* name) {
    u64 token = 0;
    if (!strcmp(name, "nvnCommandBufferEndRecording") || !strcmp(name, "nvnQueueFlush") ||
        !strcmp(name, "nvnQueuePresentTexture"))
        return 0;
    if (!strcmp(name, "nvnQueueSubmitCommands")) {
        u32 count = (u32)c->x[1];
        u64 ptr = c->x[2];
        if (count > 4096 || (count && !readable_range(ptr, (u64)count * 8))) {
            pm_lock(&lock);
            invalid_arrays++;
            ReplayQueue* queue = descriptor_replay ? replay_queue_slot(c->x[0]) : NULL;
            if (queue && queue->key == c->x[0]) {
                queue->observed_clear_only = 0;
                queue->last_unmodeled_write = queue->next_sequence++;
            }
            pm_unlock(&lock);
            return 0;
        }
        pm_lock(&lock);
        ReplayQueue* queue = descriptor_replay ? replay_queue_slot(c->x[0]) : NULL;
        if (queue && !queue->key) {
            queue->key = c->x[0];
            queue->observed_clear_only = 1;
        }
        ReplayState state = queue && queue->key ? queue->state : (ReplayState){0};
        if (descriptor_replay && !queue) replay_queue_full++;
        if (queue) {
            if (!queue->pending_roots) queue->query_value_count = 0;
            /* Cross-batch shadow images check CPU and GPU backing-memory writes before reuse. Without that
               opt-in check, a submit boundary remains an unknown write to every NVN image. */
            const char* cross = getenv("SNX_NVN_CROSS_BATCH");
            if (!cross || strcmp(cross, "1")) queue->last_unmodeled_write = queue->next_sequence++;
            queue->pending_roots += count;
        }
        for (u32 i = 0; i < count; i++) {
            u64 handle = gr64(ptr + (u64)i * 8);
            NvnList* list = list_slot(handle, 0);
            submitted++;
            if (!list) {
                submitted_missing++;
                if (queue) {
                    queue->observed_clear_only = 0;
                    queue->last_unmodeled_write = queue->next_sequence++;
                }
                continue;
            }
            submitted_found++;
            if (list->opaque) submitted_opaque++;
            for (u32 j = 0; j < list->count; j++) submitted_ops[list->ops[j].kind]++;
            if (descriptor_replay) {
                replay_roots++;
                replay_list(handle, &state, queue, 0);
            }
        }
        if (queue) {
            queue->key = c->x[0];
            queue->state = state;
        }
        pm_unlock(&lock);
        return 0;
    }
    pm_lock(&lock);
    NvnList* r = active_slot(c->x[0], 1);
    if (!r) {
        pm_unlock(&lock);
        return 0;
    }
    if (inlined_draw_replay && r->key == c->x[0] && r->opaque && r->command_cursor &&
        readable_range(c->x[0], 8)) {
        u64 cursor = gr64(c->x[0]);
        if (r->command_cursor >= r->command_low && cursor <= r->command_high && cursor > r->command_cursor) {
            u64 gap_size = cursor - r->command_cursor;
            if (gap_size > 65536 || (gap_size & 3) || !readable_range(r->command_cursor, gap_size) ||
                !nvn_command_gap_no_writes((const u32*)GP(r->command_cursor), (u32)(gap_size / 4)))
                r->command_gaps_safe = 0;
            int parsed_writes = 0;
            if (gap_size <= 65536 && !(gap_size & 3) && readable_range(r->command_cursor, gap_size)) {
                NvnGapMemoryWrite writes[32];
                u32 write_count = 0;
                if (nvn_command_gap_memory_writes((const u32*)GP(r->command_cursor), (u32)(gap_size / 4),
                                                  &r->memory_state, writes, 32, &write_count)) {
                    parsed_writes = 1;
                    for (u32 i = 0; i < write_count; i++) {
                        u64 args[7] = {writes[i].address, writes[i].size, writes[i].kind,
                                       writes[i].value, writes[i].value_known};
                        append_inlined_op(r, OP_TRANSFER, "nvnInlinedMemoryWrite", args);
                    }
                }
            }
            if (!parsed_writes) memset(&r->memory_state, 0, sizeof r->memory_state);
            const char* state_opt = getenv("SNX_NVN_INLINED_STATE");
            if ((!state_opt || strcmp(state_opt, "0")) && gap_size <= 65536 && !(gap_size & 3) &&
                readable_range(r->command_cursor, gap_size) &&
                nvn_command_gap_methods((const u32*)GP(r->command_cursor), (u32)(gap_size / 4),
                                        capture_gap_method, r))
                flush_gap_vertex_binds(r);
            u64 args[7] = {0};
            inlined_gaps++;
            if (inlined_indexed_draw(r->command_cursor, cursor - r->command_cursor, args))
                append_inlined_draw(r, args);
            else
                inlined_unparsed++;
        } else if (cursor != r->command_cursor) {
            r->command_gaps_safe = 0;
            memset(&r->memory_state, 0, sizeof r->memory_state);
        }
    }
    if (!strcmp(name, "nvnCommandBufferBeginRecording")) {
        if (r->key && r->key != UINT64_MAX && r->key != c->x[0]) active_collisions++;
        reset(r);
        r->key = c->x[0];
        r->record_generation = next_record_generation++;
        began++;
    } else {
        int op_kind = kind(name);
        captured[op_kind]++;
        if (r->key != c->x[0]) {
            if (r->key && r->key != UINT64_MAX) active_collisions++;
            reset(r);
            r->key = c->x[0];
            r->record_generation = next_record_generation++;
            r->opaque = 1; /* command-buffer activity without a visible BeginRecording */
        }
        if (inlined_draw_replay && !strcmp(name, "nvnCommandBufferAddCommandMemory")) {
            r->command_memory_count++;
            NvnPoolSnapshot pool = {0};
            u64 offset = c->x[2], size = c->x[3];
            if (nvn_pool_snapshot(c->x[1], &pool) && pool.storage && offset <= pool.size &&
                size <= pool.size - offset && pool.storage <= UINT64_MAX - offset - size) {
                r->command_low = pool.storage + offset;
                r->command_high = r->command_low + size;
                r->command_cursor = 0;
                r->command_gaps_safe = 1;
            }
        }
        if (r->opaque && !r->implicit_counted && op_kind != OP_MEMORY) {
            implicit_begins++;
            r->implicit_counted = 1;
        }
        if (op_kind == OP_CALL) {
            calls++;
            u32 count = (u32)c->x[1];
            u64 ptr = c->x[2];
            if (count > 4096 || (count && !readable_range(ptr, (u64)count * 8)))
                invalid_arrays++;
            else
                for (u32 i = 0; i < count; i++) {
                    u64 handle = gr64(ptr + (u64)i * 8);
                    NvnList* list = list_slot(handle, 0);
                    call_refs++;
                    if (!list)
                        call_ref_missing++;
                    else if (list->opaque)
                        call_ref_opaque++;
                    if (list)
                        for (u32 j = 0; j < list->count; j++) referenced_ops[list->ops[j].kind]++;
                }
        }
        if (r->key == c->x[0] && r->key) {
            if (r->count == MAX_OPS) {
                r->truncated = 1;
                dropped_ops++;
            } else {
                if (r->count == r->capacity) {
                    u16 next = r->capacity ? r->capacity * 2 : 16;
                    NvnOp* grown = realloc(r->ops, (size_t)next * sizeof *grown);
                    if (grown) {
                        r->ops = grown;
                        r->capacity = next;
                    } else {
                        allocation_failures++;
                        r->truncated = 1;
                        dropped_ops++;
                    }
                }
                if (r->count < r->capacity) {
                    u16 op_index = r->count++;
                    NvnOp* op = &r->ops[op_index];
                    memset(op, 0, sizeof *op);
                    token = ((u64)r->record_generation << 16) | ((u64)op_index + 1);
                    op->kind = op_kind;
                    op->name = name;
                    memcpy(op->args, &c->x[1], sizeof op->args);
                    snapshot_semantic(op, c);
                    if (op_kind == OP_CALL) {
                        u32 count = (u32)c->x[1];
                        if (count > 4096 || (count && !readable_range(c->x[2], (u64)count * 8))) {
                            op->refs_truncated = 1;
                        } else {
                            u64* refs = count > 8 ? malloc((size_t)count * sizeof *refs) : op->refs;
                            if (!refs) {
                                allocation_failures++;
                                op->refs_truncated = 1;
                            } else {
                                op->ref_count = (u16)count;
                                if (count > 8) op->payload = refs;
                                for (u32 i = 0; i < count; i++) refs[i] = gr64(c->x[2] + (u64)i * 8);
                            }
                        }
                        truncated_refs += op->refs_truncated;
                    }
                }
            }
        }
    }
    pm_unlock(&lock);
    return token;
}

typedef struct {
    NvnClearBatch* clears;
    NvnCopyBatch* copies;
    NvnDrawBatch* draws;
    NvnDispatchBatch* dispatches;
    NvnMemoryWriteBatch* memory_writes;
    NvnStorageSeed* storage_seeds;
    NvnUniformUpdate* uniform_updates;
} NvnGpuBatch;

static void free_storage_seeds(NvnStorageSeed* seed) {
    while (seed) {
        NvnStorageSeed* next = seed->next;
        free(seed);
        seed = next;
    }
}
static void free_uniform_updates(NvnUniformUpdate* update) {
    while (update) {
        NvnUniformUpdate* next = update->next;
        free(update);
        update = next;
    }
}

static void execute_nvn_batch(void* arg) {
    NvnGpuBatch* batch = arg;
    if (g_renderer->nvn_batch_begin) g_renderer->nvn_batch_begin();
    u32 clear = 0, copy = 0, draw = 0, dispatch = 0, memory_write = 0;
    u32 clear_count = batch->clears ? batch->clears->count : 0;
    u32 copy_count = batch->copies ? batch->copies->count : 0;
    u32 draw_count = batch->draws ? batch->draws->count : 0;
    u32 dispatch_count = batch->dispatches ? batch->dispatches->count : 0;
    u32 write_count = batch->memory_writes ? batch->memory_writes->count : 0;
    while (clear < clear_count || copy < copy_count || draw < draw_count || dispatch < dispatch_count ||
           memory_write < write_count) {
        u64 clear_seq = clear < clear_count ? batch->clears->commands[clear].sequence : UINT64_MAX;
        u64 copy_seq = copy < copy_count ? batch->copies->commands[copy].sequence : UINT64_MAX;
        u64 draw_seq = draw < draw_count ? batch->draws->commands[draw].sequence : UINT64_MAX;
        u64 dispatch_seq = dispatch < dispatch_count ? batch->dispatches->commands[dispatch].sequence : UINT64_MAX;
        u64 write_seq = memory_write < write_count ? batch->memory_writes->commands[memory_write].sequence : UINT64_MAX;
        if (write_seq < clear_seq && write_seq < copy_seq && write_seq < draw_seq && write_seq < dispatch_seq) {
            g_renderer->nvn_memory_write(&batch->memory_writes->commands[memory_write++]);
            continue;
        }
        if (clear_seq < copy_seq && clear_seq < draw_seq && clear_seq < dispatch_seq) {
            int result = g_renderer->nvn_clear(&batch->clears->commands[clear++]);
            if (result > 0) {
                __atomic_fetch_add(&clear_issued, 1, __ATOMIC_RELAXED);
                const NvnTextureTarget* target = &batch->clears->commands[clear - 1].target;
                if (target->target == 4) __atomic_fetch_add(&clear_array_issued, 1, __ATOMIC_RELAXED);
                if (target->target == 8) __atomic_fetch_add(&clear_cube_issued, 1, __ATOMIC_RELAXED);
                if (target->view.base_level) __atomic_fetch_add(&clear_mip_issued, 1, __ATOMIC_RELAXED);
            } else
                __atomic_fetch_add(&clear_unsupported, 1, __ATOMIC_RELAXED);
            if (result == -1) __atomic_fetch_add(&clear_image_full, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (copy_seq < draw_seq && copy_seq < dispatch_seq) {
            const NvnCopyBufferTextureCommand* packet = &batch->copies->commands[copy++];
            int result = g_renderer->nvn_copy_buffer_texture(packet);
            if (result == 2) __atomic_fetch_add(&texture_blit_issued, 1, __ATOMIC_RELAXED);
            u64* counter = packet->scoped_texture_write
                               ? (result > 0 ? &scoped_copy_issued : &scoped_copy_unsupported)
                               : (result > 0 ? &copy_issued : &copy_unsupported);
            __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
            continue;
        }
        if (dispatch_seq < draw_seq) {
            g_renderer->nvn_dispatch(&batch->dispatches->commands[dispatch++]);
            continue;
        }
        int result = g_renderer->nvn_draw_prepare(&batch->draws->commands[draw++]);
        if (result & NVN_DRAW_PREPARED)
            __atomic_fetch_add(&draw_prepared, 1, __ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&draw_prepare_unsupported, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_SHARED_TARGET) __atomic_fetch_add(&draw_shared_target, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TARGET_NO_SURFACE) __atomic_fetch_add(&draw_target_no_surf, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TARGET_FORMAT) __atomic_fetch_add(&draw_target_format, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TARGET_SIZE) __atomic_fetch_add(&draw_target_size, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TARGET_VIEW) __atomic_fetch_add(&draw_target_view, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_ATTACHMENT_READY) __atomic_fetch_add(&draw_attachment_ready, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_ATTACHMENT_FAILED) __atomic_fetch_add(&draw_attachment_failed, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_SHADER_MODULE_READY) __atomic_fetch_add(&draw_shader_module_ready, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_IMAGE_STORAGE_READY) __atomic_fetch_add(&draw_image_storage_ready, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_SHADER_MODULE_FAILED) __atomic_fetch_add(&draw_shader_module_failed, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_ZERO_BIND_TEXTURELESS)
            __atomic_fetch_add(&draw_zero_bind_textureless, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_ZERO_BIND_TEXTURED) __atomic_fetch_add(&draw_zero_bind_textured, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_ZERO_BIND_ALL_BINDLESS)
            __atomic_fetch_add(&draw_zero_bind_all_bindless, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_UNUSED) __atomic_fetch_add(&draw_texture_unused, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_SLOTS_MAPPED) __atomic_fetch_add(&draw_texture_slots_mapped, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_VERTEX_FORMAT_READY) __atomic_fetch_add(&draw_vertex_format_ready, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_VERTEX_FORMAT_FAILED) __atomic_fetch_add(&draw_vertex_format_failed, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_PIPELINE_READY) __atomic_fetch_add(&draw_pipeline_ready, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_PIPELINE_FAILED) __atomic_fetch_add(&draw_pipeline_failed, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_SAMPLER_READY) __atomic_fetch_add(&draw_sampler_ready, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_SAMPLER_MISSING) __atomic_fetch_add(&draw_sampler_missing, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_SAMPLER_FAILED) __atomic_fetch_add(&draw_sampler_failed, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_HEADER_READY) __atomic_fetch_add(&draw_texture_header_ready, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_HEADER_MISSING)
            __atomic_fetch_add(&draw_texture_header_missing, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_HEADER_UNSUPPORTED)
            __atomic_fetch_add(&draw_texture_header_unsupported, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_IMAGE_READY) __atomic_fetch_add(&draw_texture_image_ready, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_IMAGE_SEMANTIC)
            __atomic_fetch_add(&draw_texture_image_semantic, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_IMAGE_FAILED) __atomic_fetch_add(&draw_texture_image_failed, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_IMAGE_DEPTH) __atomic_fetch_add(&draw_texture_image_depth, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_IMAGE_MISSING)
            __atomic_fetch_add(&draw_texture_image_missing, 1, __ATOMIC_RELAXED);
        if (result & NVN_DRAW_TEXTURE_IMAGE_STALE) __atomic_fetch_add(&draw_texture_image_stale, 1, __ATOMIC_RELAXED);
    }
    free(batch->clears);
    free(batch->copies);
    free(batch->draws);
    free(batch->dispatches);
    free(batch->memory_writes);
    free_storage_seeds(batch->storage_seeds);
    free_uniform_updates(batch->uniform_updates);
    free(batch);
}

void nvn_record_after(const CPU* c, const char* name, u64 command_buffer, u64 record_token) {
    if (inlined_draw_replay && !strncmp(name, "nvnCommandBuffer", 16) &&
        strcmp(name, "nvnCommandBufferEndRecording") && readable_range(command_buffer, 8)) {
        pm_lock(&lock);
        NvnList* list = active_slot(command_buffer, 0);
        if (list && list->key == command_buffer && list->command_low < list->command_high) {
            u64 cursor = gr64(command_buffer);
            if (record_token && !strcmp(name, "nvnCommandBufferBindStorageBuffer") &&
                list->record_generation == (u32)(record_token >> 16) &&
                (u16)record_token && (u16)record_token <= list->count &&
                !strcmp(list->ops[(u16)record_token - 1].name, name) && list->command_cursor &&
                cursor > list->command_cursor && cursor <= list->command_high &&
                cursor - list->command_cursor <= 65536 && !((cursor - list->command_cursor) & 3) &&
                readable_range(list->command_cursor, cursor - list->command_cursor)) {
                NvnOp* op = &list->ops[(u16)record_token - 1];
                NvnGapMacroData3 packet;
                if (nvn_command_gap_macro_data3((const u32*)GP(list->command_cursor),
                                                (u32)((cursor - list->command_cursor) / 4), 0xe20, &packet)) {
                    u64 address = op->args[2], size = op->args[3];
                    if (packet.words[0] == (u32)address && packet.words[1] == (u32)(address >> 32) &&
                        size <= UINT32_MAX && packet.words[2] == (u32)size) {
                        op->storage_cbuf_word = packet.macro_argument;
                        op->storage_cbuf_word_known = 1;
                        storage_bind_packets++;
                    } else storage_bind_mismatches++;
                }
            }
            list->command_cursor = cursor >= list->command_low && cursor <= list->command_high ? cursor : 0;
            if (!strcmp(name, "nvnCommandBufferAddCommandMemory"))
                list->command_prefix_known = list->command_cursor == list->command_low;
        }
        pm_unlock(&lock);
    }
    if (!strcmp(name, "nvnQueueFlush") || !strcmp(name, "nvnQueuePresentTexture")) {
        if (!descriptor_replay) return;
        u64 codes[8];
        u32 count = 0;
        NvnClearBatch* clear_batch = NULL;
        NvnCopyBatch* copy_batch = NULL;
        NvnDrawBatch* draw_batch = NULL;
        NvnDispatchBatch* dispatch_batch = NULL;
        NvnMemoryWriteBatch* memory_write_batch = NULL;
        NvnStorageSeed* storage_seeds = NULL;
        NvnUniformUpdate* uniform_updates = NULL;
        pm_lock(&lock);
        ReplayQueue* queue = replay_queue_slot(command_buffer);
        if (queue && queue->key == command_buffer) {
            if (queue->pending_roots) {
                preflight_batches++;
                if (!queue->pending_clears)
                    preflight_no_clears++;
                else if (queue->observed_clear_only) {
                    preflight_clear_only++;
                    preflight_clear_commands += queue->pending_clears;
                } else
                    preflight_mixed++;
                queue->pending_roots = queue->pending_clears = 0;
                queue->observed_clear_only = 1;
            }
            if (shader_transport) {
                count = queue->vertex_probe_count;
                memcpy(codes, queue->vertex_probe_code, (size_t)count * sizeof(u64));
                for (u32 i = 0; i < queue->shader_count && count < 8; i++) {
                    u64 code = queue->shader_code[i];
                    int seen = 0;
                    for (u32 j = 0; j < count; j++) seen |= codes[j] == code;
                    if (!seen) codes[count++] = code;
                }
                queue->shader_count = 0;
                queue->vertex_probe_count = 0;
                shader_flushed += count;
            }
            if (queue->clears) {
                clear_flushed += queue->clears->count;
                if (clear_execute && queue->clears->count)
                    clear_batch = queue->clears;
                else
                    free(queue->clears);
                queue->clears = NULL;
            }
            if (queue->copies) {
                copy_flushed += queue->copies->count;
                if (copy_execute && queue->copies->count)
                    copy_batch = queue->copies;
                else
                    free(queue->copies);
                queue->copies = NULL;
            }
            if (queue->draws) {
                draw_flushed += queue->draws->count;
                if (draw_prepare && queue->draws->count)
                    draw_batch = queue->draws;
                else
                    free(queue->draws);
                queue->draws = NULL;
            }
            if (queue->dispatches) {
                if (dispatch_execute && queue->dispatches->count)
                    dispatch_batch = queue->dispatches;
                else
                    free(queue->dispatches);
                queue->dispatches = NULL;
            }
            memory_write_batch = queue->memory_writes;
            queue->memory_writes = NULL;
            storage_seeds = queue->storage_seeds;
            queue->storage_seeds = NULL;
            queue->storage_seed_bytes = 0;
            uniform_updates = queue->uniform_updates;
            queue->uniform_updates = NULL;
            queue->uniform_update_bytes = 0;
            queue->uniform_update_failed = 0;
        }
        pm_unlock(&lock);
        for (u32 i = 0; i < count; i++) nvn_program_translate_async(codes[i]);
        if (clear_batch || copy_batch || draw_batch || dispatch_batch || memory_write_batch) {
            NvnGpuBatch* owned = malloc(sizeof *owned);
            if (owned) {
                owned->clears = clear_batch;
                owned->copies = copy_batch;
                owned->draws = draw_batch;
                owned->dispatches = dispatch_batch;
                owned->memory_writes = memory_write_batch;
                owned->storage_seeds = storage_seeds;
                owned->uniform_updates = uniform_updates;
                gpu_call(execute_nvn_batch, owned);
            } else {
                free(clear_batch);
                free(copy_batch);
                free(draw_batch);
                free(dispatch_batch);
                free(memory_write_batch);
                free_storage_seeds(storage_seeds);
                free_uniform_updates(uniform_updates);
                __atomic_fetch_add(&allocation_failures, 1, __ATOMIC_RELAXED);
            }
        } else {
            free_storage_seeds(storage_seeds);
            free_uniform_updates(uniform_updates);
        }
        return;
    }
    if (strcmp(name, "nvnCommandBufferEndRecording")) return;
    pm_lock(&lock);
    ended++;
    NvnList* r = active_slot(command_buffer, 0);
    NvnList* list = list_slot(c->x[0], 1);
    if (!list) {
        if (r) {
            reset(r);
            r->key = UINT64_MAX;
        }
        pm_unlock(&lock);
        return;
    }
    reset(list);
    if (r) {
        if (inlined_draw_replay && r->command_cursor && readable_range(command_buffer, 8)) {
            u64 end = gr64(command_buffer);
            if (r->command_memory_count == 1 && end > r->command_low && end <= r->command_high &&
                end - r->command_low <= 65536 && !((end - r->command_low) & 3) &&
                readable_range(r->command_low, end - r->command_low)) {
                r->cbuf_macro_data_known = nvn_command_gap_macro_data3(
                    (const u32*)GP(r->command_low), (u32)((end - r->command_low) / 4),
                    0xe20, &r->cbuf_macro_data);
                cbuf_macro_packets += r->cbuf_macro_data_known;
            }
            if (end == r->command_cursor)
                r->command_tail_known = 1;
            else if (end > r->command_cursor && end <= r->command_high &&
                     end - r->command_cursor <= 65536 && !((end - r->command_cursor) & 3) &&
                     readable_range(r->command_cursor, end - r->command_cursor))
                r->command_tail_known = nvn_command_gap_no_writes(
                    (const u32*)GP(r->command_cursor), (u32)((end - r->command_cursor) / 4));
        }
        *list = *r;
        for (u32 j = 0; j < list->count; j++) ended_ops[list->ops[j].kind]++;
        if (list->opaque) ended_opaque++;
        if (list->truncated) truncated_lists++;
        memset(r, 0, sizeof *r);
        r->key = UINT64_MAX;
    } else {
        list->opaque = 1;
        ended_opaque++;
    }
    list->key = c->x[0];
    pm_unlock(&lock);
}

static void report_ops(const char* stage, const u64 counts[OP_COUNT]) {
    LOG("nvn-record: %s draw=%llu clear=%llu dispatch=%llu call=%llu state=%llu memory=%llu transfer=%llu barrier=%llu "
        "sync=%llu other=%llu\n",
        stage, (unsigned long long)counts[OP_DRAW], (unsigned long long)counts[OP_CLEAR],
        (unsigned long long)counts[OP_DISPATCH], (unsigned long long)counts[OP_CALL],
        (unsigned long long)counts[OP_STATE], (unsigned long long)counts[OP_MEMORY],
        (unsigned long long)counts[OP_TRANSFER], (unsigned long long)counts[OP_BARRIER],
        (unsigned long long)counts[OP_SYNC], (unsigned long long)counts[OP_OTHER]);
}

void nvn_record_report(void) {
    pm_lock(&lock);
    LOG("nvn-record: begin=%llu implicit_begin=%llu end=%llu opaque_end=%llu submit=%llu found=%llu missing=%llu "
        "opaque_submit=%llu\n",
        (unsigned long long)began, (unsigned long long)implicit_begins, (unsigned long long)ended,
        (unsigned long long)ended_opaque, (unsigned long long)submitted, (unsigned long long)submitted_found,
        (unsigned long long)submitted_missing, (unsigned long long)submitted_opaque);
    report_ops("captured", captured);
    report_ops("ended", ended_ops);
    report_ops("submitted", submitted_ops);
    report_ops("referenced", referenced_ops);
    if (inlined_draw_replay)
        LOG("nvn-record: inlined-gaps=%llu indexed-draws=%llu uniform-binds=%llu unknown-uniforms=%llu vertex-binds=%llu unknown-vertices=%llu unparsed-gaps=%llu cbuf-macro-packets=%llu\n",
            (unsigned long long)inlined_gaps, (unsigned long long)inlined_draws,
            (unsigned long long)inlined_uniform_binds, (unsigned long long)inlined_uniform_unknown,
            (unsigned long long)inlined_vertex_binds, (unsigned long long)inlined_vertex_unknown,
            (unsigned long long)inlined_unparsed, (unsigned long long)cbuf_macro_packets);
    if (inlined_draw_replay)
        LOG("nvn-record: storage-bind-packets=%llu mismatches=%llu invalid=%llu draws-with-storage=%llu\n",
            (unsigned long long)storage_bind_packets, (unsigned long long)storage_bind_mismatches,
            (unsigned long long)storage_bind_invalid, (unsigned long long)storage_draw_snapshots);
    LOG("nvn-record: refs=%llu missing_refs=%llu opaque_refs=%llu truncated_refs=%llu\n", (unsigned long long)call_refs,
        (unsigned long long)call_ref_missing, (unsigned long long)call_ref_opaque, (unsigned long long)truncated_refs);
    LOG("nvn-record: calls=%llu active_collisions=%llu list_collisions=%llu truncated_lists=%llu dropped_ops=%llu "
        "invalid_arrays=%llu alloc_failures=%llu\n",
        (unsigned long long)calls, (unsigned long long)active_collisions, (unsigned long long)list_collisions,
        (unsigned long long)truncated_lists, (unsigned long long)dropped_ops, (unsigned long long)invalid_arrays,
        (unsigned long long)allocation_failures);
    if (descriptor_replay) {
        LOG("nvn-clear-snapshot: color=%llu depth=%llu failed=%llu targets=%llu failed-targets=%llu\n",
            (unsigned long long)clear_snapshots, (unsigned long long)depth_snapshots,
            (unsigned long long)clear_snapshot_failures, (unsigned long long)target_snapshots,
            (unsigned long long)target_snapshot_failures);
        LOG("nvn-view-snapshot: layout=%u copied=%llu failed=%llu\n", nvn_texture_view_layout(),
            (unsigned long long)view_snapshots, (unsigned long long)view_snapshot_failures);
        LOG("nvn-view-overrides: levels=%llu layers=%llu format=%llu swizzle=%llu depth-mode=%llu target=%llu\n",
            (unsigned long long)view_flags[0], (unsigned long long)view_flags[1], (unsigned long long)view_flags[2],
            (unsigned long long)view_flags[3], (unsigned long long)view_flags[4], (unsigned long long)view_flags[5]);
        LOG("nvn-clear-replay: color=%llu target-resolved=%llu missing-target=%llu view-unknown=%llu stale-target=%llu\n",
            (unsigned long long)replay_color_clears, (unsigned long long)replay_color_target_resolved,
            (unsigned long long)replay_color_missing_target, (unsigned long long)replay_color_view_unknown,
            (unsigned long long)replay_color_stale_target);
        LOG("nvn-clear-replay: texture=%llu target-resolved=%llu missing-target=%llu\n",
            (unsigned long long)replay_texture_clears, (unsigned long long)replay_texture_target_resolved,
            (unsigned long long)replay_texture_missing_target);
        LOG("nvn-depth-replay: clears=%llu target-resolved=%llu missing-target=%llu view-unknown=%llu stale-target=%llu\n",
            (unsigned long long)replay_depth_clears, (unsigned long long)replay_depth_target_resolved,
            (unsigned long long)replay_depth_missing_target, (unsigned long long)replay_depth_view_unknown,
            (unsigned long long)replay_depth_stale_target);
        LOG("nvn-target-replay: stale-pool=%llu no-address=%llu bad-view=%llu\n",
            (unsigned long long)replay_target_stale_pool, (unsigned long long)replay_target_no_address,
            (unsigned long long)replay_target_bad_view);
        LOG("nvn-replay: roots=%llu lists=%llu opaque=%llu complete-opaque=%llu missing-lists=%llu truncated-refs=%llu depth-limit=%llu queue-full=%llu\n",
            (unsigned long long)replay_roots, (unsigned long long)replay_lists, (unsigned long long)replay_opaque,
            (unsigned long long)replay_complete_opaque,
            (unsigned long long)replay_missing_lists, (unsigned long long)replay_truncated_refs,
            (unsigned long long)replay_depth_limit, (unsigned long long)replay_queue_full);
        LOG("nvn-replay: binds=%llu resolved=%llu missing-pool=%llu missing-descriptor=%llu stale=%llu\n",
            (unsigned long long)replay_binds, (unsigned long long)replay_resolved,
            (unsigned long long)replay_missing_pool, (unsigned long long)replay_missing_descriptor,
            (unsigned long long)replay_stale);
        LOG("nvn-replay-missing-handles: zero=%llu nonzero=%llu\n", (unsigned long long)replay_missing_zero_handle,
            (unsigned long long)replay_missing_nonzero_handle);
        LOG("nvn-replay-sampler: resolved=%llu missing-pool=%llu missing-descriptor=%llu stale=%llu\n",
            (unsigned long long)replay_sampler_resolved, (unsigned long long)replay_sampler_missing_pool,
            (unsigned long long)replay_sampler_missing_descriptor, (unsigned long long)replay_sampler_stale);
        LOG("nvn-replay-program: binds=%llu resolved=%llu missing=%llu unconfigured=%llu\n",
            (unsigned long long)replay_program_binds, (unsigned long long)replay_program_resolved,
            (unsigned long long)replay_program_missing, (unsigned long long)replay_program_unconfigured);
        LOG("nvn-replay-program: refs-known-binds=%llu shader-refs=%llu\n",
            (unsigned long long)replay_program_refs_known, (unsigned long long)replay_program_shader_refs);
        if (shader_transport)
            LOG("nvn-shader-transport: staged=%llu flushed=%llu stage-full=%llu\n", (unsigned long long)shader_staged,
                (unsigned long long)shader_flushed, (unsigned long long)shader_stage_full);
        if (clear_execute)
            LOG("nvn-clear-execute: staged=%llu flushed=%llu issued=%llu unsupported=%llu stage-full=%llu array=%llu cube=%llu mip=%llu\n",
                (unsigned long long)clear_staged, (unsigned long long)clear_flushed,
                (unsigned long long)__atomic_load_n(&clear_issued, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&clear_unsupported, __ATOMIC_RELAXED),
                (unsigned long long)clear_stage_full,
                (unsigned long long)__atomic_load_n(&clear_array_issued, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&clear_cube_issued, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&clear_mip_issued, __ATOMIC_RELAXED));
        if (clear_execute)
            LOG("nvn-clear-image: cache-full=%llu\n",
                (unsigned long long)__atomic_load_n(&clear_image_full, __ATOMIC_RELAXED));
        if (copy_execute)
            LOG("nvn-copy-buffer-texture: captured=%llu snapshot-failed=%llu staged=%llu flushed=%llu issued=%llu "
                "unsupported=%llu stage-full=%llu\n",
                (unsigned long long)copy_snapshots, (unsigned long long)copy_snapshot_failures,
                (unsigned long long)copy_staged, (unsigned long long)copy_flushed,
                (unsigned long long)__atomic_load_n(&copy_issued, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&copy_unsupported, __ATOMIC_RELAXED),
                (unsigned long long)copy_stage_full);
        if (copy_execute)
            LOG("nvn-copy-texture-scoped: staged=%llu issued=%llu blit=%llu unsupported=%llu\n",
                (unsigned long long)scoped_copy_staged,
                (unsigned long long)__atomic_load_n(&scoped_copy_issued, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&texture_blit_issued, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&scoped_copy_unsupported, __ATOMIC_RELAXED));
        LOG("nvn-preflight: batches=%llu observed-clear-only=%llu mixed=%llu no-clears=%llu clear-commands=%llu\n",
            (unsigned long long)preflight_batches, (unsigned long long)preflight_clear_only,
            (unsigned long long)preflight_mixed, (unsigned long long)preflight_no_clears,
            (unsigned long long)preflight_clear_commands);
        LOG("nvn-draw-snapshot: vertex-ranges=%llu failed=%llu invalid-binds=%llu\n",
            (unsigned long long)vertex_range_snapshots, (unsigned long long)vertex_range_snapshot_failures,
            (unsigned long long)vertex_bind_invalid);
        LOG("nvn-draw-replay: arrays=%llu elements=%llu base-vertex=%llu instanced=%llu other=%llu program=%llu shader-refs=%llu vertex=%llu no-vertex=%llu invalid-index=%llu\n",
            (unsigned long long)draw_arrays, (unsigned long long)draw_elements, (unsigned long long)draw_base_vertex,
            (unsigned long long)draw_instanced, (unsigned long long)draw_other, (unsigned long long)draw_with_program,
            (unsigned long long)draw_with_shader_refs, (unsigned long long)draw_with_vertex,
            (unsigned long long)draw_without_vertex, (unsigned long long)draw_index_invalid);
        LOG("nvn-draw-dynamic: viewport=%llu scissor=%llu depth-range=%llu\n", (unsigned long long)draw_viewport_known,
            (unsigned long long)draw_scissor_known, (unsigned long long)draw_depth_range_known);
        LOG("nvn-draw-blend-stencil: blend-color=%llu value-mask=%llu write-mask=%llu ref=%llu\n",
            (unsigned long long)draw_blend_color_known, (unsigned long long)draw_stencil_value_mask_known,
            (unsigned long long)draw_stencil_write_mask_known, (unsigned long long)draw_stencil_ref_known);
        LOG("nvn-draw-raster: line-width=%llu polygon-offset=%llu depth-bounds=%llu sample-mask=%llu discard=%llu "
            "depth-clamp=%llu\n",
            (unsigned long long)draw_line_width_known, (unsigned long long)draw_polygon_offset_known,
            (unsigned long long)draw_depth_bounds_known, (unsigned long long)draw_sample_mask_known,
            (unsigned long long)draw_rasterizer_discard_known, (unsigned long long)draw_depth_clamp_known);
        LOG("nvn-draw-stage: staged=%llu flushed=%llu stage-full=%llu\n", (unsigned long long)draw_staged,
            (unsigned long long)draw_flushed, (unsigned long long)draw_stage_full);
        if (draw_prepare)
            LOG("nvn-draw-prepare: prepared=%llu unsupported=%llu shared-target=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_prepared, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_prepare_unsupported, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_shared_target, __ATOMIC_RELAXED));
        if (draw_prepare)
            LOG("nvn-draw-target: no-surface=%llu format=%llu size=%llu view=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_target_no_surf, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_target_format, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_target_size, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_target_view, __ATOMIC_RELAXED));
        if (attachment_prepare)
            LOG("nvn-draw-attachment: ready=%llu failed=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_attachment_ready, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_attachment_failed, __ATOMIC_RELAXED));
        if (shader_materialize)
            LOG("nvn-draw-shader-module: ready=%llu failed=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_shader_module_ready, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_shader_module_failed, __ATOMIC_RELAXED));
        if (shader_materialize)
            LOG("nvn-draw-zero-bind: shader-textureless=%llu shader-textured=%llu all-bindless=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_zero_bind_textureless, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_zero_bind_textured, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_zero_bind_all_bindless, __ATOMIC_RELAXED));
        if (shader_materialize)
            LOG("nvn-draw-texture-slots: mapped=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_texture_slots_mapped, __ATOMIC_RELAXED));
        if (shader_materialize)
            LOG("nvn-draw-vertex-format: ready=%llu failed=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_vertex_format_ready, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_vertex_format_failed, __ATOMIC_RELAXED));
        if (pipeline_prepare)
            LOG("nvn-draw-pipeline: ready=%llu failed=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_pipeline_ready, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_pipeline_failed, __ATOMIC_RELAXED));
        if (draw_prepare)
            LOG("nvn-draw-sampler: ready=%llu missing=%llu failed=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_sampler_ready, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_sampler_missing, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_sampler_failed, __ATOMIC_RELAXED));
        if (draw_prepare)
            LOG("nvn-draw-texture-header: ready=%llu missing=%llu unsupported=%llu unused=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_texture_header_ready, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_texture_header_missing, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_texture_header_unsupported, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_texture_unused, __ATOMIC_RELAXED));
        if (texture_load)
            LOG("nvn-draw-texture-image: ready=%llu failed=%llu depth-memory=%llu missing=%llu stale=%llu "
                "semantic-source=%llu\n",
                (unsigned long long)__atomic_load_n(&draw_texture_image_ready, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_texture_image_failed, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_texture_image_depth, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_texture_image_missing, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_texture_image_stale, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&draw_texture_image_semantic, __ATOMIC_RELAXED));
        LOG("nvn-uniform-snapshot: ranges=%llu failed=%llu invalid-binds=%llu\n",
            (unsigned long long)uniform_range_snapshots, (unsigned long long)uniform_range_snapshot_failures,
            (unsigned long long)uniform_bind_invalid);
        LOG("nvn-uniform-replay: draws-with-binding=%llu without=%llu\n", (unsigned long long)draw_with_uniform,
            (unsigned long long)draw_without_uniform);
        LOG("nvn-texture-snapshot: handles=%llu failed=%llu invalid-binds=%llu\n",
            (unsigned long long)texture_handle_snapshots, (unsigned long long)texture_handle_snapshot_failures,
            (unsigned long long)texture_bind_invalid);
        LOG("nvn-texture-replay: draws-with-binding=%llu without=%llu\n", (unsigned long long)draw_with_texture,
            (unsigned long long)draw_without_texture);
        LOG("nvn-image-replay: handles=%llu failed=%llu invalid-binds=%llu draws-with-binding=%llu "
            "descriptors-complete=%llu incomplete=%llu targets-complete=%llu targets-incomplete=%llu\n",
            (unsigned long long)image_handle_snapshots, (unsigned long long)image_handle_snapshot_failures,
            (unsigned long long)image_bind_invalid, (unsigned long long)draw_with_image,
            (unsigned long long)draw_image_descriptors_complete,
            (unsigned long long)draw_image_descriptors_incomplete,
            (unsigned long long)draw_image_targets_complete, (unsigned long long)draw_image_targets_incomplete);
        LOG("nvn-image-storage: ready-draws=%llu\n",
            (unsigned long long)__atomic_load_n(&draw_image_storage_ready, __ATOMIC_RELAXED));
        LOG("nvn-texture-draw: descriptors-complete=%llu incomplete=%llu\n",
            (unsigned long long)draw_texture_descriptors_complete,
            (unsigned long long)draw_texture_descriptors_incomplete);
        LOG("nvn-texture-tic-draw: complete=%llu incomplete=%llu\n", (unsigned long long)draw_texture_tics_complete,
            (unsigned long long)draw_texture_tics_incomplete);
        LOG("nvn-texture-view-draw: complete=%llu incomplete=%llu\n", (unsigned long long)draw_texture_views_complete,
            (unsigned long long)draw_texture_views_incomplete);
        LOG("nvn-sampler-draw: descriptors-complete=%llu incomplete=%llu\n",
            (unsigned long long)draw_sampler_descriptors_complete,
            (unsigned long long)draw_sampler_descriptors_incomplete);
        LOG("nvn-sampler-tsc-draw: complete=%llu incomplete=%llu\n", (unsigned long long)draw_sampler_tscs_complete,
            (unsigned long long)draw_sampler_tscs_incomplete);
        LOG("nvn-sampler-state-draw: filter-wrap-complete=%llu incomplete=%llu\n",
            (unsigned long long)draw_sampler_filter_wrap_complete,
            (unsigned long long)draw_sampler_filter_wrap_incomplete);
        LOG("nvn-program-stages: zero-mask=%llu invalid-mask=%llu draws-vertex=%llu draws-fragment=%llu\n",
            (unsigned long long)program_mask_zero, (unsigned long long)program_mask_invalid,
            (unsigned long long)draw_vertex_program, (unsigned long long)draw_fragment_program);
        LOG("nvn-vertex-state: attrib-snapshots=%llu stream-snapshots=%llu failed=%llu draws-attrib=%llu draws-stream=%llu\n",
            (unsigned long long)vertex_attrib_snapshots, (unsigned long long)vertex_stream_snapshots,
            (unsigned long long)vertex_state_snapshot_failures, (unsigned long long)draw_with_attrib_state,
            (unsigned long long)draw_with_stream_state);
        LOG("nvn-vertex-layout: enabled-attribs=%llu missing-stream-or-buffer=%llu\n",
            (unsigned long long)draw_enabled_attribs, (unsigned long long)draw_bad_attrib_stream);
        LOG("nvn-draw-noop: zero-count=%llu zero-instances=%llu\n", (unsigned long long)draw_zero_count,
            (unsigned long long)draw_zero_instances);
        LOG("nvn-draw-targets: complete=%llu incomplete=%llu none=%llu\n", (unsigned long long)draw_targets_complete,
            (unsigned long long)draw_targets_incomplete, (unsigned long long)draw_targets_none);
    }
    pm_unlock(&lock);
}
