/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_record.h"
#include "gpu/nvn_descriptor.h"
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
    u8 kind;
    u8 ref_count, refs_truncated;
    const char* name; /* stable name in the resolver's process-lifetime table */
    u64 args[7];      /* x1..x7 at the NVN call boundary */
    u64 refs[8];      /* CallCommands list handles, copied before the guest may reuse its array */
    void* payload;    /* owned snapshot for pointer arguments used by semantic replay */
} NvnOp;
typedef struct {
    u8 bytes[36];
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
} ReplayState;
typedef struct {
    u64 key;
    ReplayState state;
    u64 shader_code[8];
    u8 shader_count;
    NvnClearCommand clears[8];
    u8 clear_count;
} ReplayQueue;
#define REPLAY_QUEUE_SLOTS 1024
static ReplayQueue replay_queues[REPLAY_QUEUE_SLOTS];
static int descriptor_replay;
static int shader_transport;
static int clear_execute;
static u32 view_layout;
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
static u64 clear_staged, clear_flushed, clear_stage_full, clear_issued, clear_unsupported;

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

static ReplayQueue* replay_queue_slot(u64 key) {
    if (!key) return NULL;
    u32 home = slot(key, REPLAY_QUEUE_SLOTS - 1);
    for (u32 step = 0; step < REPLAY_QUEUE_SLOTS; step++) {
        ReplayQueue* q = &replay_queues[(home + step) & (REPLAY_QUEUE_SLOTS - 1)];
        if (q->key == key || !q->key) return q;
    }
    return NULL;
}

/* Walk a diagnostic list in GPU command order. Secondary lists inherit both descriptor-pool selections. */
static void replay_list(u64 handle, ReplayState* state, ReplayQueue* queue, u32 depth) {
    if (depth == 32) {
        replay_depth_limit++;
        return;
    }
    NvnList* list = list_slot(handle, 0);
    if (!list) {
        replay_missing_lists++;
        return;
    }
    replay_lists++;
    if (list->opaque) replay_opaque++;
    for (u32 i = 0; i < list->count; i++) {
        const NvnOp* op = &list->ops[i];
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
            } else if ((state->targets.color_views & (1u << clear->index)) &&
                       !state->targets.color_view[clear->index].valid) {
                replay_color_view_unknown++;
            } else {
                const NvnView* view = &state->targets.color_view[clear->index];
                NvnTextureTarget target;
                NvnTargetResult result = nvn_texture_resolve_target(
                    state->targets.color[clear->index],
                    (state->targets.color_views & (1u << clear->index)) ? &view->selection : NULL, &target);
                if (result == NVN_TARGET_RESOLVED) {
                    replay_color_target_resolved++;
                    if (clear_execute && queue) {
                        if (queue->clear_count == 8) {
                            clear_stage_full++;
                        } else {
                            NvnClearCommand* packet = &queue->clears[queue->clear_count++];
                            *packet = (NvnClearCommand){.kind = NVN_CLEAR_COLOR, .target = target, .mask = clear->mask};
                            memcpy(packet->color, clear->color, sizeof packet->color);
                            clear_staged++;
                        }
                    }
                } else if (result == NVN_TARGET_STALE_POOL)
                    replay_target_stale_pool++;
                else if (result == NVN_TARGET_NO_ADDRESS)
                    replay_target_no_address++;
                else if (result == NVN_TARGET_BAD_VIEW)
                    replay_target_bad_view++;
                else
                    replay_color_stale_target++;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferClearDepthStencil")) {
            replay_depth_clears++;
            if (!op->payload || !state->targets.valid || !state->targets.depth) {
                replay_depth_missing_target++;
            } else if (state->targets.depth_view && !state->targets.depth_view_data.valid) {
                replay_depth_view_unknown++;
            } else {
                NvnTextureTarget target;
                NvnTargetResult result = nvn_texture_resolve_target(
                    state->targets.depth, state->targets.depth_view ? &state->targets.depth_view_data.selection : NULL,
                    &target);
                if (result == NVN_TARGET_RESOLVED) {
                    replay_depth_target_resolved++;
                    if (clear_execute && queue) {
                        if (queue->clear_count == 8) {
                            clear_stage_full++;
                        } else {
                            const NvnClearDepth* clear = op->payload;
                            NvnClearCommand* packet = &queue->clears[queue->clear_count++];
                            *packet = (NvnClearCommand){.kind = NVN_CLEAR_DEPTH_STENCIL,
                                                        .target = target,
                                                        .depth_bits = clear->depth,
                                                        .stencil = clear->stencil,
                                                        .depth_mask = clear->depth_mask,
                                                        .stencil_mask = clear->stencil_mask};
                            clear_staged++;
                        }
                    }
                } else if (result == NVN_TARGET_STALE_POOL)
                    replay_target_stale_pool++;
                else if (result == NVN_TARGET_NO_ADDRESS)
                    replay_target_no_address++;
                else if (result == NVN_TARGET_BAD_VIEW)
                    replay_target_bad_view++;
                else
                    replay_depth_stale_target++;
            }
        } else if (!strcmp(op->name, "nvnCommandBufferSetTexturePool")) {
            state->texture_pool = op->args[0];
        } else if (!strcmp(op->name, "nvnCommandBufferSetSamplerPool")) {
            state->sampler_pool = op->args[0];
        } else if (!strcmp(op->name, "nvnCommandBufferBindTexture")) {
            replay_binds++;
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
        } else if (!strcmp(op->name, "nvnCommandBufferBindProgram")) {
            replay_program_binds++;
            NvnProgramSnapshot program = {0};
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
        } else if (!strcmp(op->name, "nvnCommandBufferCallCommands")) {
            if (op->refs_truncated) replay_truncated_refs++;
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
    u32 layout = 0;
    if (first_instruction == 0xa9007c1f && second_instruction == 0xd65f03c0) /* 16-byte packed view */
        layout = 16;
    else if (first_instruction == 0x6f00e400 && second_instruction == 0xb900201f &&
             third_instruction == 0xad000000) /* 36-byte field view */
        layout = 36;
    __atomic_store_n(&view_layout, layout, __ATOMIC_RELEASE);
}

static void snapshot_view(u64 ptr, NvnView* view) {
    u32 layout = __atomic_load_n(&view_layout, __ATOMIC_ACQUIRE);
    if (!layout || !readable_range(ptr, layout)) {
        view_snapshot_failures++;
        return;
    }
    for (u32 i = 0; i < layout; i += 4) {
        u32 word = gr32(ptr + i);
        memcpy(view->bytes + i, &word, 4);
    }
    if (layout == 16) {
        u64 packed;
        memcpy(&packed, view->bytes, sizeof packed);
        view->selection.flags = (packed >> 56) & 0x3f;
        view->selection.base_level = (packed >> 32) & 0xff;
        view->selection.level_count = (packed >> 40) & 0xff;
        view->selection.first_layer = packed & 0xffff;
        view->selection.layer_count = (packed >> 16) & 0xffff;
        memcpy(&view->selection.format, view->bytes + 8, 4);
        view->selection.target = (packed >> 48) & 0xff;
    } else {
        view->selection.flags = view->bytes[0] & 0x3f;
        view->selection.base_level = view->bytes[1];
        view->selection.level_count = view->bytes[2];
        memcpy(&view->selection.first_layer, view->bytes + 4, 2);
        memcpy(&view->selection.layer_count, view->bytes + 6, 2);
        memcpy(&view->selection.format, view->bytes + 8, 4);
        memcpy(&view->selection.target, view->bytes + 32, 4);
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
            pm_unlock(&lock);
            return;
        }
        pm_lock(&lock);
        ReplayQueue* queue = descriptor_replay ? replay_queue_slot(c->x[0]) : NULL;
        ReplayState state = queue && queue->key ? queue->state : (ReplayState){0};
        if (descriptor_replay && !queue) replay_queue_full++;
        for (u32 i = 0; i < count; i++) {
            u64 handle = gr64(ptr + (u64)i * 8);
            NvnList* list = list_slot(handle, 0);
            submitted++;
            if (!list) {
                submitted_missing++;
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
    u32 count;
    NvnClearCommand commands[8];
} NvnClearBatch;

static void execute_clear_batch(void* arg) {
    NvnClearBatch* batch = arg;
    for (u32 i = 0; i < batch->count; i++) {
        if (g_renderer->nvn_clear(&batch->commands[i]))
            __atomic_fetch_add(&clear_issued, 1, __ATOMIC_RELAXED);
        else
            __atomic_fetch_add(&clear_unsupported, 1, __ATOMIC_RELAXED);
    }
    free(batch);
}

void nvn_record_after(const CPU* c, const char* name, u64 command_buffer) {
    if (!strcmp(name, "nvnQueueFlush") || !strcmp(name, "nvnQueuePresentTexture")) {
        if (!shader_transport && !clear_execute) return;
        u64 codes[8];
        u32 count = 0;
        NvnClearBatch batch = {0};
        pm_lock(&lock);
        ReplayQueue* queue = replay_queue_slot(command_buffer);
        if (queue && queue->key == command_buffer) {
            if (shader_transport) {
                count = queue->shader_count;
                memcpy(codes, queue->shader_code, (size_t)count * sizeof(u64));
                queue->shader_count = 0;
                shader_flushed += count;
            }
            if (clear_execute) {
                batch.count = queue->clear_count;
                memcpy(batch.commands, queue->clears, (size_t)batch.count * sizeof(NvnClearCommand));
                queue->clear_count = 0;
                clear_flushed += batch.count;
            }
        }
        pm_unlock(&lock);
        for (u32 i = 0; i < count; i++) nvn_program_translate_async(codes[i]);
        if (batch.count) {
            NvnClearBatch* owned = malloc(sizeof *owned);
            if (owned) {
                *owned = batch;
                gpu_call(execute_clear_batch, owned);
            } else {
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
        LOG("nvn-view-snapshot: layout=%u copied=%llu failed=%llu\n", __atomic_load_n(&view_layout, __ATOMIC_ACQUIRE),
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
    }
    pm_unlock(&lock);
}
