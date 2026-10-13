/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_program.h"
#include "memory/mem.h"
#include "gpu/gpu.h"
#ifdef SNX_VULKAN
#include "gpu/shader/shader.h"
#include "gpu/nvn_shader.h"
#include "gpu/vk/spirv.h"
#endif
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <stdlib.h>
#include <string.h>

/* Program identity and setup status only; shader descriptors vary between inspected SDKs. */
typedef struct {
    u64 key, generation;
    u32 shader_count;
    u8 alive, configured, refs_known;
    NvnShaderRef refs[NVN_PROGRAM_MAX_SHADERS];
} Program;
#define PROGRAM_SLOTS 65536
static Program programs[PROGRAM_SLOTS];
static PMutex lock = PMUTEX_INIT;
static u64 initialized, init_failed, finalized, live, reinitialized, unknown_finalizes;
static u64 shader_sets, shader_set_failed, shader_set_untracked, table_full;
static u64 refs_captured, refs_invalid, refs_oversized, zero_code_addresses, zero_control_addresses;
static u32 shader_translate_queued;
static int shader_translate_enabled;
static u64 shader_translate_ok, shader_translate_failed, shader_translate_unknown_stage, shader_translate_bad_blob;
#ifdef SNX_VULKAN
static PMutex translate_lock = PMUTEX_INIT;
static u64 translated_codes[8192];
static void shader_translate_probe(void* ptr);

static int reserve_shader_slot(void) {
    u32 count = __atomic_load_n(&shader_translate_queued, __ATOMIC_RELAXED);
    while (count < 8) {
        if (__atomic_compare_exchange_n(&shader_translate_queued, &count, count + 1, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
            return 1;
    }
    return 0;
}

void nvn_program_translate_async(u64 code_address) {
    if (!shader_translate_enabled || !code_address || !reserve_shader_slot()) return;
    pm_lock(&translate_lock);
    u32 home = (u32)(code_address * 2654435761u) & (8192 - 1);
    u32 step = 0;
    int fresh = 0;
    for (; step < 8192; step++) {
        u64* slot = &translated_codes[(home + step) & (8192 - 1)];
        if (*slot == code_address) break;
        if (!*slot) {
            *slot = code_address;
            fresh = 1;
            break;
        }
    }
    pm_unlock(&translate_lock);
    if (!fresh) {
        __atomic_fetch_sub(&shader_translate_queued, 1, __ATOMIC_RELAXED);
        return;
    }
    u64* address = malloc(sizeof *address);
    if (!address) {
        __atomic_fetch_add(&shader_translate_failed, 1, __ATOMIC_RELAXED);
        __atomic_fetch_sub(&shader_translate_queued, 1, __ATOMIC_RELAXED);
        return;
    }
    *address = code_address;
    gpu_call(shader_translate_probe, address);
}

/* A bounded execution probe: translate real NVN shader blobs through the existing Maxwell and SPIR-V path on the
   GPU thread. It intentionally has no constant-buffer reader, so its output is not used for game rendering. */
static void shader_translate_probe(void* ptr) {
    u64 address = *(u64*)ptr;
    u8* code = malloc(NVN_SHADER_CODE_MAX);
    if (code) {
        NvnShaderMeta meta = {0};
        if (!nvn_shader_load(address, code, &meta)) {
            __atomic_fetch_add(&shader_translate_bad_blob, 1, __ATOMIC_RELAXED);
            u32 header[4] = {0};
            gmem_read_code(address, header, sizeof header);
            WARN_LIMIT(8, "nvn-shader-translate: bad blob code=%llx header=%08x %08x %08x %08x\n",
                       (unsigned long long)address, header[0], header[1], header[2], header[3]);
        } else if (meta.stage < 0) {
            __atomic_fetch_add(&shader_translate_unknown_stage, 1, __ATOMIC_RELAXED);
        } else {
            ShaderInfo info = {0};
            int (*old)(u32, u32, u32*) = g_shader_cb_read;
            g_shader_cb_read = NULL;
            char* glsl = shader_to_glsl(code, NVN_SHADER_CODE_MAX, meta.stage, &info);
            g_shader_cb_read = old;
            glsl = vk_glsl_post(glsl, meta.stage, shader_flushes_denorms(&info));
            size_t words = 0;
            char err[512] = "";
            u32* spirv = glsl ? vk_spirv_cached(glsl, meta.stage, NULL, &words, err, sizeof err) : NULL;
            if (spirv && !info.unsupported)
                __atomic_fetch_add(&shader_translate_ok, 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_add(&shader_translate_failed, 1, __ATOMIC_RELAXED);
            LOG("nvn-shader-translate: code=%llx stage=%d inputs=%08x unsupported=%d spirv-words=%zu error=%s\n",
                (unsigned long long)address, meta.stage, info.in_mask, info.unsupported, words, spirv ? "" : err);
            free(spirv);
            free(glsl);
        }
        free(code);
    } else {
        __atomic_fetch_add(&shader_translate_failed, 1, __ATOMIC_RELAXED);
    }
    free(ptr);
    __atomic_fetch_sub(&shader_translate_queued, 1, __ATOMIC_RELAXED);
}
#else
void nvn_program_translate_async(u64 code_address) { (void)code_address; }
#endif

void nvn_program_enable_translation(void) { shader_translate_enabled = 1; }

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

static Program* program_slot(u64 key, int create) {
    if (!key) return NULL;
    u32 home = (u32)((key >> 4) * 2654435761u) & (PROGRAM_SLOTS - 1);
    for (u32 step = 0; step < PROGRAM_SLOTS; step++) {
        Program* p = &programs[(home + step) & (PROGRAM_SLOTS - 1)];
        if (p->key == key) return p;
        if (!p->key) return create ? p : NULL;
    }
    return NULL;
}

int nvn_program_wants(const char* name) {
    return !strcmp(name, "nvnProgramInitialize") || !strcmp(name, "nvnProgramFinalize") ||
           !strcmp(name, "nvnProgramSetShaders") || !strcmp(name, "nvnProgramSetShadersExt");
}

void nvn_program_before(const CPU* c, const char* name, NvnProgramCall* call) {
    memset(call, 0, sizeof *call);
    call->program = c->x[0];
    call->shader_count =
        !strcmp(name, "nvnProgramSetShaders") || !strcmp(name, "nvnProgramSetShadersExt") ? (u32)c->x[1] : 0;
    if (!call->shader_count || call->shader_count > NVN_PROGRAM_MAX_SHADERS) return;
    if (!strcmp(name, "nvnProgramSetShadersExt")) {
        if (!readable_range(c->x[2], (u64)call->shader_count * sizeof(NvnShaderRef))) return;
        memcpy(call->refs, GP(c->x[2]), (size_t)call->shader_count * sizeof(NvnShaderRef));
    } else if (!strcmp(name, "nvnProgramSetShaders")) {
        /* The plain entry is a 16-byte code/control pair. The Ext wrapper copies those same two
           words plus an optional third word from each 24-byte entry before calling shared setup. */
        u64 pair[NVN_PROGRAM_MAX_SHADERS][2];
        if (!readable_range(c->x[2], (u64)call->shader_count * sizeof pair[0])) return;
        memcpy(pair, GP(c->x[2]), (size_t)call->shader_count * sizeof pair[0]);
        for (u32 i = 0; i < call->shader_count; i++) {
            call->refs[i].code_candidate = pair[i][0];
            call->refs[i].control_candidate = pair[i][1];
        }
    } else {
        return;
    }
    call->refs_valid = 1;
}

void nvn_program_after(const CPU* c, const char* name, const NvnProgramCall* call) {
    pm_lock(&lock);
    if (!strcmp(name, "nvnProgramInitialize")) {
        if (!c->x[0]) {
            init_failed++;
        } else {
            Program* p = program_slot(call->program, 1);
            if (!p)
                table_full++;
            else {
                if (p->alive)
                    reinitialized++;
                else
                    live++;
                p->key = call->program;
                p->generation++;
                p->shader_count = 0;
                p->configured = 0;
                p->refs_known = 0;
                p->alive = 1;
                initialized++;
            }
        }
    } else if (!strcmp(name, "nvnProgramFinalize")) {
        Program* p = program_slot(call->program, 0);
        if (!p || !p->alive)
            unknown_finalizes++;
        else {
            p->alive = 0;
            live--;
            finalized++;
        }
    } else {
        Program* p = program_slot(call->program, 0);
        if (!p || !p->alive)
            shader_set_untracked++;
        else if (!c->x[0])
            shader_set_failed++;
        else {
            p->configured = 1;
            p->shader_count = call->shader_count;
            p->refs_known = call->refs_valid;
            if (call->refs_valid) {
                memcpy(p->refs, call->refs, (size_t)call->shader_count * sizeof(NvnShaderRef));
                refs_captured += call->shader_count;
                for (u32 i = 0; i < call->shader_count; i++) {
                    if (!call->refs[i].code_candidate) zero_code_addresses++;
                    if (!call->refs[i].control_candidate) zero_control_addresses++;
                }
            } else if (call->shader_count > NVN_PROGRAM_MAX_SHADERS) {
                refs_oversized++;
            } else {
                refs_invalid++;
            }
            shader_sets++;
        }
    }
    pm_unlock(&lock);
}

int nvn_program_check(u64 program) {
    pm_lock(&lock);
    Program* p = program_slot(program, 0);
    int result = p && p->alive ? (p->configured ? 1 : -1) : 0;
    pm_unlock(&lock);
    return result;
}

int nvn_program_snapshot(u64 program, NvnProgramSnapshot* out) {
    pm_lock(&lock);
    Program* p = program_slot(program, 0);
    int found = p && p->alive && p->configured;
    if (found) {
        out->generation = p->generation;
        out->shader_count = p->shader_count;
        out->refs_known = p->refs_known;
        if (p->refs_known) memcpy(out->refs, p->refs, (size_t)p->shader_count * sizeof(NvnShaderRef));
    }
    pm_unlock(&lock);
    return found;
}

void nvn_program_report(void) {
    pm_lock(&lock);
    LOG("nvn-program: initialized=%llu init-failed=%llu finalized=%llu live=%llu reinitialized=%llu unknown-finalizes=%llu shader-sets=%llu shader-set-failed=%llu shader-set-untracked=%llu table-full=%llu\n",
        (unsigned long long)initialized, (unsigned long long)init_failed, (unsigned long long)finalized,
        (unsigned long long)live, (unsigned long long)reinitialized, (unsigned long long)unknown_finalizes,
        (unsigned long long)shader_sets, (unsigned long long)shader_set_failed,
        (unsigned long long)shader_set_untracked, (unsigned long long)table_full);
    LOG("nvn-program: refs-captured=%llu invalid-ref-arrays=%llu oversized-ref-arrays=%llu zero-code-addresses=%llu zero-control-addresses=%llu\n",
        (unsigned long long)refs_captured, (unsigned long long)refs_invalid, (unsigned long long)refs_oversized,
        (unsigned long long)zero_code_addresses, (unsigned long long)zero_control_addresses);
    if (shader_translate_enabled)
        LOG("nvn-shader-translate: queued=%u compiled=%llu failed=%llu unknown-stage=%llu bad-blob=%llu\n",
            __atomic_load_n(&shader_translate_queued, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&shader_translate_ok, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&shader_translate_failed, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&shader_translate_unknown_stage, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&shader_translate_bad_blob, __ATOMIC_RELAXED));
    pm_unlock(&lock);
}
