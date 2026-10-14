/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_program.h"
#include "memory/mem.h"
#include "gpu/gpu.h"
#include "gpu/shader/shader.h"
#include "gpu/shader/translator.h"
#include "gpu/nvn_shader.h"
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
static PMutex translate_lock = PMUTEX_INIT;
static u64 translated_codes[8192];
typedef struct {
    u64 address, hash;
    u32 size;
    u16 writes;
    u8 state; /* 0 available, 1 queued, 2 certified, 3 rejected for these code bytes */
} ComputeWrites;
#define COMPUTE_WRITES_SLOTS 4096
static ComputeWrites compute_writes[COMPUTE_WRITES_SLOTS];
static PMutex compute_writes_lock = PMUTEX_INIT;
static void shader_translate_probe(void* ptr);

static ComputeWrites* compute_writes_slot(u64 address, int create) {
    u32 home = (u32)((address >> 4) * 2654435761u) & (COMPUTE_WRITES_SLOTS - 1);
    for (u32 i = 0; i < COMPUTE_WRITES_SLOTS; i++) {
        ComputeWrites* slot = &compute_writes[(home + i) & (COMPUTE_WRITES_SLOTS - 1)];
        if (slot->address == address) return slot;
        if (!slot->address) {
            if (create) slot->address = address;
            return create ? slot : NULL;
        }
    }
    return NULL;
}

static u64 compute_writes_hash(const u8* code, u32 size) {
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i < size; i++) hash = (hash ^ code[i]) * 1099511628211ull;
    return hash;
}

/* Translate with the same 80-byte SPH prefix as the NVN QMD path. A BRX needs live CBUF
   jump-table words, so program-setup analysis never certifies a shader containing one. */
static void analyze_compute_writes(void* ptr) {
    u64 address = *(u64*)ptr;
    free(ptr);
    u32 header[2] = {0};
    gmem_read_code(address, header, sizeof header);
    u32 size = header[1], length = 0;
    u16 writes = 0;
    u64 hash = 0;
    int certified = 0, repeatable = 0;
    int bounded = 0, mapped = 0, branch = -1, unsupported = -1, images = -1, buffers = -1;
    const char* branch_name = "";
    if (header[0] == 0x12345679 && size >= 0x80 && size <= NVN_SHADER_CODE_MAX &&
        address <= UINT64_MAX - 0x100 - size) {
        u8* code = malloc((size_t)size + 80);
        if (code) {
            memset(code, 0, 80);
            gmem_read_code(address + 0x100, code + 80, size);
            bounded = mapped = !!gmem_ptr_untracked(address + 0x100, size);
            if (mapped) hash = compute_writes_hash(code + 80, size);
            int (*old)(u32, u32, u32*) = g_shader_cb_read;
            g_shader_cb_read = NULL;
            shader_hash(code, size + 80, &length);
            bounded &= length >= 80 && length <= size + 80;
            for (u32 i = 11; bounded && i < length / 8; i++) {
                if (!IDX_OK(i)) continue;
                const Op* op = decode(((const u64*)code)[i]);
                /* The translator skips unknown encodings, which cannot prove a write set. */
                if (!op || op->id == O_BRX || op->id == O_CAL || op->id == O_JMP) {
                    bounded = 0;
                    branch = (int)i;
                    branch_name = op ? op->name : "UNKNOWN";
                }
            }
            if (mapped && !bounded) repeatable = 1;
            if (bounded) {
                ShaderInfo info = {.cs_local = {1, 1, 1}};
                size_t words = 0;
                u32* spirv = shader_to_spirv(code, size + 80, ST_CS, &info, &words, 1, 1, 1);
                if (spirv) repeatable = 1;
                unsupported = info.unsupported;
                images = info.nimg;
                buffers = info.ngbuf;
                if (spirv && !info.unsupported && !info.nimg && info.ngbuf <= 16) {
                    certified = 1;
                    for (u32 i = 0; i < (u32)info.ngbuf; i++) {
                        if (!(info.gbuf_wmask & (1u << i))) continue;
                        u32 offset = info.gbuf_cbuf_off[i];
                        if (offset < 0x310 || offset >= 0x410 || ((offset - 0x310) & 15)) {
                            certified = 0;
                            break;
                        }
                        writes |= (u16)(1u << ((offset - 0x310) / 16));
                    }
                }
                free(spirv);
            }
            g_shader_cb_read = old;
            free(code);
        }
    }
    const char* trace = getenv("SNX_NVN_DISPATCH_TRACE");
    if (trace && !strcmp(trace, "1"))
        LOG("nvn-compute-analysis: code=%llx magic=%08x size=%x length=%x mapped=%d bounded=%d branch=%d/%s unsupported=%d images=%d buffers=%d writes=%04x certified=%d\n",
            (unsigned long long)address, header[0], size, length, mapped, bounded, branch, branch_name, unsupported, images, buffers,
            writes, certified);
    pm_lock(&compute_writes_lock);
    ComputeWrites* slot = compute_writes_slot(address, 0);
    if (slot && slot->state == 1) {
        slot->hash = hash;
        slot->size = size;
        slot->writes = writes;
        slot->state = certified ? 2 : repeatable ? 3 : 0;
    }
    pm_unlock(&compute_writes_lock);
}

static void queue_compute_writes(u64 address) {
    if (!address || address > UINT64_MAX - 0x100 - NVN_SHADER_CODE_MAX) return;
    const u8* header = gmem_ptr_untracked(address, 8);
    u32 magic = 0, size = 0;
    if (!header) return;
    memcpy(&magic, header, sizeof magic);
    memcpy(&size, header + 4, sizeof size);
    if (magic != 0x12345679 || size < 0x80 || size > NVN_SHADER_CODE_MAX) return;
    const u8* code = gmem_ptr_untracked(address + 0x100, size);
    if (!code) return;
    u64 hash = compute_writes_hash(code, size);
    pm_lock(&compute_writes_lock);
    ComputeWrites* slot = compute_writes_slot(address, 1);
    int queued = slot &&
                 (!slot->state || (slot->state >= 2 && (slot->size != size || slot->hash != hash)));
    if (queued) slot->state = 1;
    pm_unlock(&compute_writes_lock);
    if (!queued) return;
    u64* owned = malloc(sizeof *owned);
    if (!owned) {
        pm_lock(&compute_writes_lock);
        slot = compute_writes_slot(address, 0);
        if (slot && slot->state == 1) slot->state = 0;
        pm_unlock(&compute_writes_lock);
        return;
    }
    *owned = address;
    gpu_call(analyze_compute_writes, owned);
}

int nvn_program_compute_write_slots(u64 address, u16* slots) {
    if (!address || !slots) return 0;
    const u8* header = gmem_ptr_untracked(address, 8);
    u32 magic = 0, size = 0;
    if (!header) return 0;
    memcpy(&magic, header, sizeof magic);
    memcpy(&size, header + 4, sizeof size);
    if (magic != 0x12345679 || size < 0x80 || size > NVN_SHADER_CODE_MAX ||
        address > UINT64_MAX - 0x100 - size) return 0;
    const u8* code = gmem_ptr_untracked(address + 0x100, size);
    if (!code) return 0;
    u64 hash = compute_writes_hash(code, size);
    pm_lock(&compute_writes_lock);
    ComputeWrites* slot = compute_writes_slot(address, 0);
    int valid = slot && slot->state == 2 && slot->size == size && slot->hash == hash;
    if (valid) *slots = slot->writes;
    int stale = !slot || !slot->state || (slot->state >= 2 && (slot->size != size || slot->hash != hash));
    const char* trace = getenv("SNX_NVN_DISPATCH_TRACE");
    if (trace && !strcmp(trace, "1"))
        LOG("nvn-compute-writes: code=%llx size=%x state=%u analyzed-size=%x writes=%04x hash-match=%d valid=%d\n",
            (unsigned long long)address, size, slot ? slot->state : 0,
            slot ? slot->size : 0, slot ? slot->writes : 0,
            slot && slot->hash == hash, valid);
    pm_unlock(&compute_writes_lock);
    if (stale) queue_compute_writes(address);
    return valid;
}

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
            size_t words = 0;
            u32* spirv = shader_to_spirv(code, NVN_SHADER_CODE_MAX, meta.stage, &info, &words, 1, 1, 1);
            g_shader_cb_read = old;
            if (spirv && !info.unsupported)
                __atomic_fetch_add(&shader_translate_ok, 1, __ATOMIC_RELAXED);
            else
                __atomic_fetch_add(&shader_translate_failed, 1, __ATOMIC_RELAXED);
            LOG("nvn-shader-translate: code=%llx stage=%d inputs=%08x unsupported=%d spirv-words=%zu error=%s\n",
                (unsigned long long)address, meta.stage, info.in_mask, info.unsupported, words, spirv ? "" : info.first_unsupported);
            free(spirv);
        }
        free(code);
    } else {
        __atomic_fetch_add(&shader_translate_failed, 1, __ATOMIC_RELAXED);
    }
    free(ptr);
    __atomic_fetch_sub(&shader_translate_queued, 1, __ATOMIC_RELAXED);
}
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
    u64 analyze[NVN_PROGRAM_MAX_SHADERS] = {0};
    u32 analyze_count = 0;
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
                    if (getenv("SNX_NVN_SHADOW_DISPATCH") && analyze_count < NVN_PROGRAM_MAX_SHADERS)
                        analyze[analyze_count++] = call->refs[i].code_candidate;
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
    for (u32 i = 0; i < analyze_count; i++) queue_compute_writes(analyze[i]);
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
