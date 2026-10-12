/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_record.h"
#include "gpu/nvn_descriptor.h"
#include "gpu/nvn_draw.h"
#include "gpu/nvn_sampler_descriptor.h"
#include "gpu/nvn_program.h"
#include "gpu/nvn_texture.h"
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
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
    u8 kind;
    u8 ref_count, refs_truncated;
    const char* name; /* stable name in the resolver's process-lifetime table */
    u64 args[7];      /* x1..x7 at the NVN call boundary */
    u64 refs[8];      /* CallCommands list handles, copied before the guest may reuse its array */
    void* payload;    /* owned snapshot for pointer arguments used by semantic replay */
} NvnOp;
typedef struct {
    NvnTextureViewSelection selection;
    u8 valid;
} NvnView;
typedef struct {
    u32 count;
    u8 color_views, depth_view, valid;
    u64 color[8], depth;
    NvnView color_view[8], depth_view_data;
} NvnTargets;
typedef struct {
    u32 color[4];
    u32 index, mask;
} NvnClearColor;
typedef struct {
    u32 depth, stencil, depth_mask, stencil_mask;
} NvnClearDepth;
typedef struct {
    u32 attrib[NVN_DRAW_VERTEX_SLOTS];
    u64 stream[NVN_DRAW_VERTEX_SLOTS];
    u8 count;
} NvnVertexState;
typedef struct {
    u64 key;
    NvnOp* ops;
    u16 count, capacity;
    u8 truncated, opaque, implicit_counted;
} NvnList;
#define ACTIVE_SLOTS 8192
#define LIST_SLOTS   32768
#define MAX_OPS      16384
static NvnList active[ACTIVE_SLOTS], lists[LIST_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 began, implicit_begins, ended, ended_opaque, submitted, submitted_found, submitted_missing, submitted_opaque;
static u64 calls, call_refs, call_ref_missing, call_ref_opaque, truncated_refs;
static u64 active_collisions, list_collisions, truncated_lists, dropped_ops, invalid_arrays, allocation_failures;
static u64 captured[OP_COUNT], ended_ops[OP_COUNT], submitted_ops[OP_COUNT], referenced_ops[OP_COUNT];
typedef struct {
    u64 texture_pool, sampler_pool;
    NvnTargets targets;
    NvnStageProgram program[NVN_DRAW_SHADER_STAGES];
    u8 program_bound;
    u16 vertex_bound;
    NvnBufferRange vertex[NVN_DRAW_VERTEX_SLOTS];
    u32 vertex_attrib_raw[NVN_DRAW_VERTEX_SLOTS];
    u64 vertex_stream_raw[NVN_DRAW_VERTEX_SLOTS];
    u8 vertex_attrib_count, vertex_stream_count;
    NvnBufferRange uniform[NVN_DRAW_SHADER_STAGES][NVN_DRAW_UNIFORM_SLOTS];
    u16 uniform_bound[NVN_DRAW_SHADER_STAGES];
    u64 texture[NVN_DRAW_SHADER_STAGES][NVN_DRAW_TEXTURE_SLOTS];
    u32 texture_bound[NVN_DRAW_SHADER_STAGES];
} ReplayState;
typedef struct {
    u64 key;
    u64 next_sequence;
    ReplayState state;
    u32 pending_roots, pending_clears;
    u8 observed_clear_only;
    u64 shader_code[8];
    u8 shader_count;
    u64 vertex_probe_code[8];
    u8 vertex_probe_count;
    NvnClearBatch* clears;
    NvnDrawBatch* draws;
} ReplayQueue;
#define REPLAY_QUEUE_SLOTS 1024
static ReplayQueue replay_queues[REPLAY_QUEUE_SLOTS];
static int descriptor_replay;
static int shader_transport;
static int clear_execute;
static int draw_prepare;
static u64 shader_staged, shader_flushed, shader_stage_full;
static u64 replay_roots, replay_lists, replay_opaque, replay_missing_lists, replay_truncated_refs, replay_depth_limit;
static u64 replay_binds, replay_resolved, replay_missing_pool, replay_missing_descriptor, replay_stale;
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
static u64 replay_depth_clears, replay_depth_target_resolved, replay_depth_missing_target, replay_depth_view_unknown,
    replay_depth_stale_target;
static u64 replay_target_stale_pool, replay_target_no_address, replay_target_bad_view;
static u64 clear_staged, clear_flushed, clear_stage_full, clear_issued, clear_unsupported, clear_image_full;
static u64 preflight_batches, preflight_clear_only, preflight_mixed, preflight_no_clears, preflight_clear_commands;
static u64 vertex_range_snapshots, vertex_range_snapshot_failures, vertex_bind_invalid;
static u64 draw_arrays, draw_elements, draw_base_vertex, draw_instanced, draw_other, draw_with_program,
    draw_with_shader_refs;
static u64 draw_with_vertex, draw_without_vertex, draw_index_invalid, draw_staged, draw_stage_full, draw_flushed;
static u64 draw_prepared, draw_prepare_unsupported, draw_shared_target, draw_target_no_surf, draw_target_format,
    draw_target_size, draw_target_view;
static u64 uniform_range_snapshots, uniform_range_snapshot_failures, uniform_bind_invalid;
static u64 draw_with_uniform, draw_without_uniform;
static u64 texture_handle_snapshots, texture_handle_snapshot_failures, texture_bind_invalid;
static u64 draw_with_texture, draw_without_texture;
static u64 draw_texture_descriptors_complete, draw_texture_descriptors_incomplete;
static u64 draw_texture_tics_complete, draw_texture_tics_incomplete;
static u64 draw_texture_views_complete, draw_texture_views_incomplete;
static u64 draw_sampler_descriptors_complete, draw_sampler_descriptors_incomplete;
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
void nvn_record_enable_draw_prepare(void) { draw_prepare = 1; }

static ReplayQueue* replay_queue_slot(u64 key) {
    if (!key) return NULL;
    u32 home = slot(key, REPLAY_QUEUE_SLOTS - 1);
    for (u32 step = 0; step < REPLAY_QUEUE_SLOTS; step++) {
        ReplayQueue* q = &replay_queues[(home + step) & (REPLAY_QUEUE_SLOTS - 1)];
        if (q->key == key || !q->key) return q;
    }
    return NULL;
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

/* Walk a diagnostic list in GPU command order. Secondary lists inherit both descriptor-pool selections. */
static void replay_list(u64 handle, ReplayState* state, ReplayQueue* queue, u32 depth) {
    if (depth == 32) {
        replay_depth_limit++;
        if (queue) queue->observed_clear_only = 0;
        return;
    }
    NvnList* list = list_slot(handle, 0);
    if (!list) {
        replay_missing_lists++;
        if (queue) queue->observed_clear_only = 0;
        return;
    }
    replay_lists++;
    if (list->opaque) replay_opaque++;
    if (queue && (list->opaque || list->truncated)) queue->observed_clear_only = 0;
    for (u32 i = 0; i < list->count; i++) {
        const NvnOp* op = &list->ops[i];
        if (queue && op->kind != OP_MEMORY && strcmp(op->name, "nvnCommandBufferSetRenderTargets") &&
            strcmp(op->name, "nvnCommandBufferClearColor") && strcmp(op->name, "nvnCommandBufferClearDepthStencil") &&
            strcmp(op->name, "nvnCommandBufferCallCommands"))
            queue->observed_clear_only = 0;
        if (!strcmp(op->name, "nvnCommandBufferSetRenderTargets")) {
            if (op->payload)
                state->targets = *(const NvnTargets*)op->payload;
            else
                memset(&state->targets, 0, sizeof state->targets);
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
                NvnTargetResult result = nvn_texture_resolve_target(
                    state->targets.color[clear->index],
                    (state->targets.color_views & (1u << clear->index)) ? &view->selection : NULL, &target);
                if (result == NVN_TARGET_RESOLVED) {
                    replay_color_target_resolved++;
                    if (queue) queue->pending_clears++;
                    if (clear_execute && queue) {
                        NvnClearCommand packet = {.kind = NVN_CLEAR_COLOR, .target = target, .mask = clear->mask};
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
                NvnTargetResult result = nvn_texture_resolve_target(
                    state->targets.depth, state->targets.depth_view ? &state->targets.depth_view_data.selection : NULL,
                    &target);
                if (result == NVN_TARGET_RESOLVED) {
                    replay_depth_target_resolved++;
                    if (queue) queue->pending_clears++;
                    if (clear_execute && queue) {
                        const NvnClearDepth* clear = op->payload;
                        NvnClearCommand packet = {.kind = NVN_CLEAR_DEPTH_STENCIL,
                                                  .target = target,
                                                  .depth_bits = clear->depth,
                                                  .stencil = clear->stencil,
                                                  .depth_mask = clear->depth_mask,
                                                  .stencil_mask = clear->stencil_mask};
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
                else
                    replay_missing_descriptor++;
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
        } else if (!strcmp(op->name, "nvnCommandBufferBindVertexBuffer")) {
            u32 index = (u32)op->args[0];
            if (index >= NVN_DRAW_VERTEX_SLOTS)
                vertex_bind_invalid++;
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
        } else if (!strcmp(op->name, "nvnCommandBufferBindUniformBuffer")) {
            u32 stage = (u32)op->args[0], index = (u32)op->args[1];
            if (stage >= NVN_DRAW_SHADER_STAGES || index >= NVN_DRAW_UNIFORM_SLOTS)
                uniform_bind_invalid++;
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
        } else if (!strcmp(op->name, "nvnCommandBufferDrawArrays") ||
                   !strcmp(op->name, "nvnCommandBufferDrawArraysInstanced") ||
                   !strcmp(op->name, "nvnCommandBufferDrawElements") ||
                   !strcmp(op->name, "nvnCommandBufferDrawElementsBaseVertex") ||
                   !strcmp(op->name, "nvnCommandBufferDrawElementsInstanced")) {
            NvnDrawCommand draw = {0};
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
            memcpy(draw.texture, state->texture, sizeof draw.texture);
            memcpy(draw.texture_bound, state->texture_bound, sizeof draw.texture_bound);
            int descriptors_complete = 1, tics_complete = 1, views_complete = 1, samplers_complete = 1,
                filter_wrap_complete = 1, has_texture = 0;
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
                        if ((draw.sampler_descriptor[stage][index].state.known &
                             (NVN_SAMPLER_FILTER_KNOWN | NVN_SAMPLER_WRAP_KNOWN)) !=
                            (NVN_SAMPLER_FILTER_KNOWN | NVN_SAMPLER_WRAP_KNOWN))
                            filter_wrap_complete = 0;
                    } else {
                        samplers_complete = 0;
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
                if (nvn_texture_resolve_target(state->targets.color[j], view, &draw.color_target[j]) ==
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
                    if (nvn_texture_resolve_target(state->targets.depth, view, &draw.depth_target) ==
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
            }
        } else if (op->kind == OP_DRAW) {
            draw_other++;
        } else if (!strcmp(op->name, "nvnCommandBufferCallCommands")) {
            if (op->refs_truncated) {
                replay_truncated_refs++;
                if (queue) queue->observed_clear_only = 0;
            }
            for (u32 j = 0; j < op->ref_count; j++) replay_list(op->refs[j], state, queue, depth + 1);
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

/* The SDK consumes these pointers immediately. Copy them before the caller can reuse its stack. */
static void snapshot_semantic(NvnOp* op, const CPU* c) {
    if (!strcmp(op->name, "nvnCommandBufferClearColor")) {
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
        }
        targets->depth = op->args[3];
        targets->depth_view = !!op->args[4];
        if (targets->depth_view) snapshot_view(op->args[4], &targets->depth_view_data);
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
    } else if (!strcmp(op->name, "nvnCommandBufferBindUniformBuffers")) {
        u32 stage = (u32)op->args[0], first = (u32)op->args[1], count = (u32)op->args[2];
        if (stage >= NVN_DRAW_SHADER_STAGES || first >= NVN_DRAW_UNIFORM_SLOTS ||
            count > NVN_DRAW_UNIFORM_SLOTS - first ||
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
    } else if (!strcmp(op->name, "nvnCommandBufferBindTextures")) {
        u32 stage = (u32)op->args[0], first = (u32)op->args[1], count = (u32)op->args[2];
        if (stage >= NVN_DRAW_SHADER_STAGES || first >= NVN_DRAW_TEXTURE_SLOTS ||
            count > NVN_DRAW_TEXTURE_SLOTS - first || (count && !readable_range(op->args[3], (u64)count * 8))) {
            texture_handle_snapshot_failures++;
            return;
        }
        u64* handles = calloc(count ? count : 1, sizeof *handles);
        if (!handles) {
            allocation_failures++;
            texture_handle_snapshot_failures++;
            return;
        }
        for (u32 i = 0; i < count; i++) handles[i] = gr64(op->args[3] + (u64)i * 8);
        op->payload = handles;
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

void nvn_record_before(const CPU* c, const char* name) {
    if (!strcmp(name, "nvnCommandBufferEndRecording") || !strcmp(name, "nvnQueueFlush") ||
        !strcmp(name, "nvnQueuePresentTexture"))
        return;
    if (!strcmp(name, "nvnQueueSubmitCommands")) {
        u32 count = (u32)c->x[1];
        u64 ptr = c->x[2];
        if (count > 4096 || (count && !readable_range(ptr, (u64)count * 8))) {
            pm_lock(&lock);
            invalid_arrays++;
            ReplayQueue* queue = descriptor_replay ? replay_queue_slot(c->x[0]) : NULL;
            if (queue && queue->key == c->x[0]) queue->observed_clear_only = 0;
            pm_unlock(&lock);
            return;
        }
        pm_lock(&lock);
        ReplayQueue* queue = descriptor_replay ? replay_queue_slot(c->x[0]) : NULL;
        if (queue && !queue->key) {
            queue->key = c->x[0];
            queue->observed_clear_only = 1;
        }
        ReplayState state = queue && queue->key ? queue->state : (ReplayState){0};
        if (descriptor_replay && !queue) replay_queue_full++;
        if (queue) queue->pending_roots += count;
        for (u32 i = 0; i < count; i++) {
            u64 handle = gr64(ptr + (u64)i * 8);
            NvnList* list = list_slot(handle, 0);
            submitted++;
            if (!list) {
                submitted_missing++;
                if (queue) queue->observed_clear_only = 0;
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
        return;
    }
    pm_lock(&lock);
    NvnList* r = active_slot(c->x[0], 1);
    if (!r) {
        pm_unlock(&lock);
        return;
    }
    if (!strcmp(name, "nvnCommandBufferBeginRecording")) {
        if (r->key && r->key != UINT64_MAX && r->key != c->x[0]) active_collisions++;
        reset(r);
        r->key = c->x[0];
        began++;
    } else {
        int op_kind = kind(name);
        captured[op_kind]++;
        if (r->key != c->x[0]) {
            if (r->key && r->key != UINT64_MAX) active_collisions++;
            reset(r);
            r->key = c->x[0];
            r->opaque = 1; /* command-buffer activity without a visible BeginRecording */
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
                    NvnOp* op = &r->ops[r->count++];
                    memset(op, 0, sizeof *op);
                    op->kind = op_kind;
                    op->name = name;
                    memcpy(op->args, &c->x[1], sizeof op->args);
                    snapshot_semantic(op, c);
                    if (op_kind == OP_CALL && (u32)c->x[1] <= 4096 &&
                        (!(u32)c->x[1] || readable_range(c->x[2], (u64)(u32)c->x[1] * 8))) {
                        u32 count = (u32)c->x[1];
                        op->ref_count = count < 8 ? count : 8;
                        op->refs_truncated = count > 8;
                        truncated_refs += count > 8;
                        for (u32 i = 0; i < op->ref_count; i++) op->refs[i] = gr64(c->x[2] + (u64)i * 8);
                    }
                }
            }
        }
    }
    pm_unlock(&lock);
}

typedef struct {
    NvnClearBatch* clears;
    NvnDrawBatch* draws;
} NvnGpuBatch;

static void execute_nvn_batch(void* arg) {
    NvnGpuBatch* batch = arg;
    u32 clear = 0, draw = 0;
    u32 clear_count = batch->clears ? batch->clears->count : 0;
    u32 draw_count = batch->draws ? batch->draws->count : 0;
    while (clear < clear_count || draw < draw_count) {
        if (clear < clear_count &&
            (draw == draw_count || batch->clears->commands[clear].sequence < batch->draws->commands[draw].sequence)) {
            int result = g_renderer->nvn_clear(&batch->clears->commands[clear++]);
            if (result > 0)
                __atomic_fetch_add(&clear_issued, 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_add(&clear_unsupported, 1, __ATOMIC_RELAXED);
            if (result == -1) __atomic_fetch_add(&clear_image_full, 1, __ATOMIC_RELAXED);
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
    }
    free(batch->clears);
    free(batch->draws);
    free(batch);
}

void nvn_record_after(const CPU* c, const char* name, u64 command_buffer) {
    if (!strcmp(name, "nvnQueueFlush") || !strcmp(name, "nvnQueuePresentTexture")) {
        if (!descriptor_replay) return;
        u64 codes[8];
        u32 count = 0;
        NvnClearBatch* clear_batch = NULL;
        NvnDrawBatch* draw_batch = NULL;
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
            if (queue->draws) {
                draw_flushed += queue->draws->count;
                if (draw_prepare && queue->draws->count)
                    draw_batch = queue->draws;
                else
                    free(queue->draws);
                queue->draws = NULL;
            }
        }
        pm_unlock(&lock);
        for (u32 i = 0; i < count; i++) nvn_program_translate_async(codes[i]);
        if (clear_batch || draw_batch) {
            NvnGpuBatch* owned = malloc(sizeof *owned);
            if (owned) {
                owned->clears = clear_batch;
                owned->draws = draw_batch;
                gpu_call(execute_nvn_batch, owned);
            } else {
                free(clear_batch);
                free(draw_batch);
                __atomic_fetch_add(&allocation_failures, 1, __ATOMIC_RELAXED);
            }
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
        LOG("nvn-depth-replay: clears=%llu target-resolved=%llu missing-target=%llu view-unknown=%llu stale-target=%llu\n",
            (unsigned long long)replay_depth_clears, (unsigned long long)replay_depth_target_resolved,
            (unsigned long long)replay_depth_missing_target, (unsigned long long)replay_depth_view_unknown,
            (unsigned long long)replay_depth_stale_target);
        LOG("nvn-target-replay: stale-pool=%llu no-address=%llu bad-view=%llu\n",
            (unsigned long long)replay_target_stale_pool, (unsigned long long)replay_target_no_address,
            (unsigned long long)replay_target_bad_view);
        LOG("nvn-replay: roots=%llu lists=%llu opaque=%llu missing-lists=%llu truncated-refs=%llu depth-limit=%llu queue-full=%llu\n",
            (unsigned long long)replay_roots, (unsigned long long)replay_lists, (unsigned long long)replay_opaque,
            (unsigned long long)replay_missing_lists, (unsigned long long)replay_truncated_refs,
            (unsigned long long)replay_depth_limit, (unsigned long long)replay_queue_full);
        LOG("nvn-replay: binds=%llu resolved=%llu missing-pool=%llu missing-descriptor=%llu stale=%llu\n",
            (unsigned long long)replay_binds, (unsigned long long)replay_resolved,
            (unsigned long long)replay_missing_pool, (unsigned long long)replay_missing_descriptor,
            (unsigned long long)replay_stale);
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
            LOG("nvn-clear-execute: staged=%llu flushed=%llu issued=%llu unsupported=%llu stage-full=%llu\n",
                (unsigned long long)clear_staged, (unsigned long long)clear_flushed,
                (unsigned long long)__atomic_load_n(&clear_issued, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(&clear_unsupported, __ATOMIC_RELAXED),
                (unsigned long long)clear_stage_full);
        if (clear_execute)
            LOG("nvn-clear-image: cache-full=%llu\n",
                (unsigned long long)__atomic_load_n(&clear_image_full, __ATOMIC_RELAXED));
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
