/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_trace.h"
#include "gpu/gpu.h"
#include "gpu/nvn_hle.h"
#include "gpu/nvn_record.h"
#include "gpu/nvn_pool.h"
#include "gpu/nvn_buffer.h"
#include "gpu/nvn_texture.h"
#include "core/loader.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include <stdlib.h>
#include <string.h>

static GuestFn bootstrap_original;
static GuestFn getproc_original;
static GuestFn texture_size_original;
static u64 texture_size_entry;
static int tracing, hle, verify, recording, pools, buffers, textures;
static u64 verify_failures;
typedef struct {
    u64 addr, calls;
    GuestFn original;
    char name[128];
} NvProc;
#define NVN_PROC_SLOTS 1024
static NvProc procs[NVN_PROC_SLOTS];
static u32 nprocs;
static void getproc_trace(CPU* c, u64 entry);

static int readable(u64 addr) {
    if (addr < AS_BASE || addr >= AS_SIZE) return 0;
    MemBlock block;
    mem_query(addr, &block);
    return block.state != MS_FREE && block.state != MS_INACCESSIBLE && (block.perm & PERM_R);
}

static int read_name(u64 ptr, char name[128]) {
    size_t i;
    for (i = 0; i < 127 && ptr + i >= ptr && readable(ptr + i); i++) {
        name[i] = *(const char*)GP(ptr + i);
        if (!name[i]) return i != 0;
    }
    name[i] = 0;
    return 0;
}

static NvProc* proc_slot(u64 addr) {
    u32 i = (u32)((addr >> 2) * 2654435761u) & (NVN_PROC_SLOTS - 1);
    while (procs[i].addr && procs[i].addr != addr) i = (i + 1) & (NVN_PROC_SLOTS - 1);
    return &procs[i];
}

static void proc_trace(CPU* c, u64 entry) {
    NvProc* p = proc_slot(entry);
    u64 command_buffer = c->x[0];
    int record_call = recording && nvn_record_wants(p->name);
    int pool_call = pools && nvn_pool_wants(p->name);
    NvnPoolCall pool;
    if (pool_call) nvn_pool_before(c, p->name, &pool);
    int buffer_call = buffers && nvn_buffer_wants(p->name);
    NvnBufferCall buffer;
    if (buffer_call) nvn_buffer_before(c, p->name, &buffer);
    int texture_call = textures && nvn_texture_wants(p->name);
    NvnTextureCall texture;
    if (texture_call) nvn_texture_before(c, p->name, &texture);
    if (record_call) nvn_record_before(c, p->name);
    int submit = tracing && !strcmp(p->name, "nvnQueueSubmitCommands");
    int queue_op = tracing && (!strcmp(p->name, "nvnQueueFlush") || !strcmp(p->name, "nvnQueueFenceSync") ||
                               !strcmp(p->name, "nvnQueuePresentTexture") || !strcmp(p->name, "nvnQueueWaitSync"));
    u64 gpu_before = submit || queue_op ? __atomic_load_n(&g_gpu_submits, __ATOMIC_RELAXED) : 0;
    if (tracing) {
        __atomic_fetch_add(&p->calls, 1, __ATOMIC_RELAXED);
        if (submit) {
            u64 list = 0, ptr = c->x[2];
            if (c->x[1] && ptr < AS_SIZE - 8 && readable(ptr) && readable(ptr + 7)) memcpy(&list, GP(ptr), 8);
            LOG("nvn-submit: queue=%llx count=%u lists=%llx first=%llx\n", (unsigned long long)c->x[0], (u32)c->x[1],
                (unsigned long long)ptr, (unsigned long long)list);
        }
    }
    if (hle) {
        CPU expected;
        if (verify) expected = *c;
        if (nvn_hle_try(c, p->name)) {
            if (verify) {
                p->original(&expected, entry);
                if (memcmp(c, &expected, sizeof expected)) {
                    __atomic_fetch_add(&verify_failures, 1, __ATOMIC_RELAXED);
                    WARN_LIMIT(8, "nvn-hle: register mismatch in %s, using the guest result\n", p->name);
                    *c = expected;
                }
            }
            return;
        }
    }
    p->original(c, entry);
    if (texture_call && !strcmp(p->name, "nvnTextureInitialize") && c->x[0] && texture.valid) {
        GuestFn texture_get_size = __atomic_load_n(&texture_size_original, __ATOMIC_ACQUIRE);
        if (texture_get_size) {
            /* Query the successful guest object through its own pure SDK getter. The inspected SDKs store this value
           at different offsets, so reading an assumed object field would silently mismeasure one of them. */
            CPU query = *c;
            query.x[0] = texture.texture;
            texture_get_size(&query, __atomic_load_n(&texture_size_entry, __ATOMIC_RELAXED));
            texture.storage_size = (u32)query.x[0];
            texture.size_known = 1;
        }
    }
    if (pool_call) nvn_pool_after(c, p->name, &pool);
    if (buffer_call) nvn_buffer_after(c, p->name, &buffer);
    if (texture_call) nvn_texture_after(c, p->name, &texture);
    if (record_call) nvn_record_after(c, p->name, command_buffer);
    if (submit || queue_op)
        LOG("nvn-%s: GPFIFO submissions during call=%llu\n", p->name,
            (unsigned long long)(__atomic_load_n(&g_gpu_submits, __ATOMIC_RELAXED) - gpu_before));
}

static void trace_proc(u64 addr, const char* name) {
    if (addr < g_code_lo || addr >= g_code_hi || (addr & 3) || nprocs == NVN_PROC_SLOTS) return;
    GuestFn* dispatch = &g_disp[(addr - g_code_lo) / 4];
    if (*dispatch == proc_trace || *dispatch == getproc_trace) return;
    NvProc* p = proc_slot(addr);
    if (p->addr) return;
    p->addr = addr;
    p->original = *dispatch ? *dispatch : snx_interp_entry;
    memcpy(p->name, name, strlen(name) + 1);
    nprocs++;
    *dispatch = proc_trace;
}

static void getproc_trace(CPU* c, u64 entry) {
    char name[128];
    int valid = read_name(c->x[1], name); /* nvnDeviceGetProcAddress(device, name) */
    getproc_original(c, entry);
    if (valid) {
        if (tracing) LOG("nvn: %s -> %llx\n", name, (unsigned long long)c->x[0]);
        if (tracing || (hle && nvn_hle_supported(name)) || (recording && nvn_record_wants(name)) ||
            (pools && nvn_pool_wants(name)) || (buffers && nvn_buffer_wants(name)) ||
            (textures && (nvn_texture_wants(name) || !strcmp(name, "nvnTextureGetStorageSize"))))
            trace_proc(c->x[0], name);
        if (textures && !strcmp(name, "nvnTextureGetStorageSize") && c->x[0] >= g_code_lo && c->x[0] < g_code_hi &&
            !(c->x[0] & 3)) {
            NvProc* getter = proc_slot(c->x[0]);
            if (getter->addr == c->x[0]) {
                __atomic_store_n(&texture_size_entry, c->x[0], __ATOMIC_RELAXED);
                __atomic_store_n(&texture_size_original, getter->original, __ATOMIC_RELEASE);
            }
        }
    }
}

static void bootstrap_trace(CPU* c, u64 entry) {
    char name[128];
    int valid = read_name(c->x[0], name); /* nvnBootstrapLoader(name) */
    bootstrap_original(c, entry);
    if (!valid) return;
    if (tracing) LOG("nvn: %s -> %llx\n", name, (unsigned long long)c->x[0]);
    if (strcmp(name, "nvnDeviceGetProcAddress")) return;
    u64 resolver = c->x[0];
    if (resolver < g_code_lo || resolver >= g_code_hi || (resolver & 3)) return;
    GuestFn* slot = &g_disp[(resolver - g_code_lo) / 4];
    if (!*slot || *slot == getproc_trace) return;
    getproc_original = *slot;
    *slot = getproc_trace;
}

void nvn_trace_init(void) {
    const char* trace_env = getenv("SNX_NVN_TRACE");
    const char* hle_env = getenv("SNX_NVN_HLE");
    const char* verify_env = getenv("SNX_NVN_HLE_VERIFY");
    const char* record_env = getenv("SNX_NVN_RECORD");
    const char* pool_env = getenv("SNX_NVN_POOL");
    const char* buffer_env = getenv("SNX_NVN_BUFFER");
    const char* texture_env = getenv("SNX_NVN_TEXTURE");
    tracing = trace_env && !strcmp(trace_env, "1");
    hle = hle_env && !strcmp(hle_env, "1");
    verify = hle && verify_env && !strcmp(verify_env, "1");
    recording = record_env && !strcmp(record_env, "1");
    buffers = buffer_env && !strcmp(buffer_env, "1");
    textures = texture_env && !strcmp(texture_env, "1");
    pools = buffers || textures || (pool_env && !strcmp(pool_env, "1"));
    if (!tracing && !hle && !recording && !pools) return;
    u64 entry = kernel_find_symbol_exact("nvnBootstrapLoader");
    if (!entry || entry < g_code_lo || entry >= g_code_hi || (entry & 3)) {
        WARN("nvn: bootstrap loader symbol is unavailable\n");
        return;
    }
    GuestFn* slot = &g_disp[(entry - g_code_lo) / 4];
    if (!*slot) {
        WARN("nvn: bootstrap loader has no recompiled entry\n");
        return;
    }
    bootstrap_original = *slot;
    *slot = bootstrap_trace;
    LOG("nvn: %s bootstrap function requests\n", tracing ? "tracing" : "intercepting");
}

void nvn_trace_report(void) {
    if (recording) nvn_record_report();
    if (pools) nvn_pool_report();
    if (buffers) nvn_buffer_report();
    if (textures) nvn_texture_report();
    if (hle) {
        nvn_hle_report();
        if (verify) LOG("nvn-hle: %llu verification mismatches\n", (unsigned long long)verify_failures);
    }
    if (!tracing || !bootstrap_original) return;
    u32 used = 0;
    for (u32 i = 0; i < NVN_PROC_SLOTS; i++) {
        if (!procs[i].addr) continue;
        u64 calls = __atomic_load_n(&procs[i].calls, __ATOMIC_RELAXED);
        if (!calls) continue;
        used++;
        LOG("nvn-used: %s %llu\n", procs[i].name, (unsigned long long)calls);
    }
    LOG("nvn: %u resolved addresses, %u called\n", nprocs, used);
}
