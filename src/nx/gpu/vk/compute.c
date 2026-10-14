/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compute: the compute class's launches (QMD), on a compute queue of their own when the device has one (a queue of a
   compute-only family, or a second queue of the graphics family), else on the graphics queue. Each dispatch gets
   copies of its constant and storage buffers in a host-visible arena and is recorded into a batch. The batch is
   submitted and its results land in guest memory before anything could observe them: a read of that memory
   (gmem_read and gmem_ptr call cs_mem_need), anything after it in the stream the game can observe (semaphore releases
   and reports: gpu_signal_point), a GPU-side write of that memory, a syncpoint increment or a wait. A byte is written
   only where the shader changed it and guest memory still holds what the shader read, since on the console the GPU
   stores while it runs and a later CPU write wins.

   A queue of its own only waits for compute, so holding signals until results land costs a dispatch's run time,
   not the rendering backlog's (which made that ordering too slow under the old OpenGL renderer, ROADMAP.md Phase 2).
   Dispatches that read textures run on the graphics queue, after everything recorded before them. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "gpu/gpu.h"
#include "hle/services/nvdrv.h"
#include "gpu/shader/shader.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include "runtime/main.h"
#include "gpu/vk/vk.h"
#include "gpu/nvn_uniform.h"
#include "gpu/renderer.h"

typedef struct CsPipe {
    u64 key;
    VkPipeline p;
    ShaderInfo si;
    VkCbReads cbr; /* the constant buffer words translation read */
    int failed;
    struct CsPipe* next;
} CsPipe;
static CsPipe* g_csp[256];

/* the arena: inputs and outputs of the batch's dispatches, and what the outputs held before (org, host only) */
static VkBuffer g_ar;
static VmaAllocation g_ar_mem;
static u8* g_ar_ptr;
static u8* g_org;
static u64 g_ar_cap, g_ar_used;
typedef struct {
    u64 addr;
    u64 off;
    u32 size;
    u64 code;
} CsOut;
static CsOut g_out[256];
static u64 cs_probe_hash(const u8* bytes, u32 size) {
    u64 hash = 1469598103934665603ull;
    for (u32 i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
typedef struct {
    u64 address, code;
    u32 size;
    int input;
    u8* bytes;
} CsShadowExpected;
static CsShadowExpected g_shadow_expected[64];
static u32 g_shadow_expected_count;

/* QMD finishes before the captured NVN batch is replayed. Retain each QMD result so a
   direct dispatch can be compared with its own result, not later guest-memory contents. */
static void cs_shadow_expect(u64 address, u32 size, u64 code, const u8* source, int input) {
    if (!address || !size || size > (1u << 20)) return;
    if (g_shadow_expected_count == 64) {
        free(g_shadow_expected[0].bytes);
        memmove(g_shadow_expected, g_shadow_expected + 1, 63 * sizeof g_shadow_expected[0]);
        g_shadow_expected_count--;
    }
    u8* bytes = malloc(size);
    if (!bytes) return;
    memcpy(bytes, source, size);
    g_shadow_expected[g_shadow_expected_count++] = (CsShadowExpected){
        .address = address, .code = code, .size = size, .input = input, .bytes = bytes};
}

void vk_compute_shadow_compare(u64 address, u32 size, u64 code, const u8* bytes, int input) {
    for (u32 i = 0; i < g_shadow_expected_count; i++) {
        CsShadowExpected expected = g_shadow_expected[i];
        /* Observed QMD code addresses follow the captured NVN program address by 0x100 bytes. */
        if (expected.address != address || expected.size != size || expected.code != code + 0x100 ||
            expected.input != input) continue;
        u32 different = 0, first = size;
        for (u32 b = 0; b < size; b++)
            if (expected.bytes[b] != bytes[b]) {
                if (first == size) first = b;
                different++;
            }
        LOG_LIMIT(16, "nvn-shadow-qmd-compare: phase=%s code=%llx qmd=%llx addr=%llx size=%u different=%u first=%u "
                      "qmd/direct=%02x/%02x\n",
                  input ? "input" : "output",
                  (unsigned long long)code, (unsigned long long)expected.code,
                  (unsigned long long)address, size, different, first,
                  first < size ? expected.bytes[first] : 0, first < size ? bytes[first] : 0);
        free(expected.bytes);
        memmove(g_shadow_expected + i, g_shadow_expected + i + 1,
                (g_shadow_expected_count - i - 1) * sizeof g_shadow_expected[0]);
        g_shadow_expected_count--;
        return;
    }
}
static void cs_trace_output(const CsOut* output) {
    const char* trace = getenv("SNX_NVN_SHADOW_INPUT_TRACE");
    if (!trace || strcmp(trace, "1")) return;
    cs_shadow_expect(output->addr, output->size, output->code, g_ar_ptr + output->off, 0);
    if (output->size != 58848) return;
    const u8* bytes = g_ar_ptr + output->off;
    LOG_LIMIT(8, "nvn-qmd-storage-output: code=%llx addr=%llx hash=%llx bytes29,304,354=%02x,%02x,%02x\n",
              (unsigned long long)output->code, (unsigned long long)output->addr,
              (unsigned long long)cs_probe_hash(bytes, output->size),
              bytes[29], bytes[304], bytes[354]);
}
static int g_nout;
static u64 g_out_lo = ~0ull, g_out_hi;

static VkCommandPool g_cpool;
static VkCommandBuffer g_ccb;
static VkFence g_cfence;
static VkDescriptorPool g_dpool;
static int g_recording, g_landing;
static int g_inflight; /* submitted, results not landed yet */
/* draws can read results on the GPU before they land (vk_compute_gpu_src): the batch signals g_vk_csem, the next
   graphics submit waits for g_vk_cs_wait_value, and the arena they read stays alive until that frame is done */
VkSemaphore g_vk_csem;
u64 g_vk_cs_wait_value;
static u64 g_cval;
static int g_ar_gfx;
static u64 g_org_cap;
u64 g_vk_dispatches, g_vk_cs_batches, g_vk_cs_skipped, g_vk_cs_wait_ns;

static u8 g_cb[8][0x10000];
static u32 g_cbn[8];
static int cs_cb_read(u32 idx, u32 off, u32* out) {
    if (idx < 8 && off + 4 <= g_cbn[idx]) {
        memcpy(out, g_cb[idx] + off, 4);
        return 1;
    }
    return 0;
}

static u64 cs_used_hash(u32 bank, const ShaderInfo* si) {
    u64 hash = 1469598103934665603ull;
    u32 size = si->cbuf_max[bank] < g_cbn[bank] ? si->cbuf_max[bank] : g_cbn[bank];
    for (u32 off = 0; off < size; off++) {
        if (bank == 0 && (off / 128 >= 13 ||
                          !(si->cbuf0_read_words[off / 128] & (1u << ((off / 4) & 31))))) continue;
        hash = (hash ^ g_cb[bank][off]) * 1099511628211ull;
    }
    return hash;
}

static void cs_init(void) {
    if (g_cpool) return;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_vk.compute_family};
    VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_cpool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_ccb));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_OK(vkCreateFence(g_vk.dev, &fci, NULL, &g_cfence));
    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_vk_csem));
    /* the dispatches of a batch: a batch that runs out lands, which resets the pool */
    VkDescriptorPoolSize sz[4];
    vk_pool_sizes(sz, 1024);
    VkDescriptorPoolCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1024, .poolSizeCount = 4, .pPoolSizes = sz};
    VK_OK(vkCreateDescriptorPool(g_vk.dev, &dci, NULL, &g_dpool));
}

/* a bigger arena; only between batches, nothing points into it then */
static u64 g_ar_want = 8ull << 20; /* doubles when a batch fills the arena, up to 64 MB */
/* arenas draws read from: reused once the frames that read them are done */
static struct {
    VkBuffer b;
    VmaAllocation m;
    u8* p;
    u64 cap, frame;
} g_retired[8];
static int g_nretired;
static void arena_retire(void) {
    if (!g_ar) return;
    if (g_nretired == 8) { /* full: the oldest goes for good */
        vk_defer_buffer(g_retired[0].b, g_retired[0].m);
        memmove(g_retired, g_retired + 1, sizeof g_retired[0] * 7);
        g_nretired--;
    }
    g_retired[g_nretired++] = (typeof(g_retired[0])){g_ar, g_ar_mem, g_ar_ptr, g_ar_cap, vk_frame_number()};
    g_ar = VK_NULL_HANDLE;
}
static int arena_fit(u64 need) {
    if (g_ar && g_ar_used <= g_ar_cap && need <= g_ar_cap - g_ar_used && g_ar_cap >= g_ar_want) return 1;
    vk_cmd(); /* frames older than VK_FRAMES are done once the current one has started */
    for (int i = 0; i < g_nretired; i++)
        if (g_retired[i].frame + VK_FRAMES <= vk_frame_number() && g_retired[i].cap >= need &&
            g_retired[i].cap >= g_ar_want) {
            if (g_ar) vk_defer_buffer(g_ar, g_ar_mem);
            g_ar = g_retired[i].b;
            g_ar_mem = g_retired[i].m;
            g_ar_ptr = g_retired[i].p;
            g_ar_cap = g_retired[i].cap;
            g_retired[i] = g_retired[--g_nretired];
            return 1;
        }
    if (g_ar) vk_defer_buffer(g_ar, g_ar_mem); /* draws may still read it */
    u64 cap = g_ar_want;
    while (cap < need) cap *= 2;
    u32 fam[2] = {g_vk.gfx_family, g_vk.compute_family};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = cap,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              /* both queues use it, one at a time, without ownership transfers */
                              .sharingMode = fam[0] != fam[1] ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                              .queueFamilyIndexCount = fam[0] != fam[1] ? 2 : 0,
                              .pQueueFamilyIndices = fam};
    VmaAllocationCreateInfo aci = {.flags =
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    g_ar = VK_NULL_HANDLE;
    g_ar_cap = 0;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_ar, &g_ar_mem, &info))) return 0;
    g_ar_ptr = info.pMappedData;
    g_ar_cap = cap;
    if (g_org_cap < cap) {
        free(g_org);
        g_org = malloc(cap);
        g_org_cap = g_org ? cap : 0;
    }
    return g_org != NULL;
}
static u64 arena_alloc(u64 size) {
    u64 al = vk_stage_align(), at = (g_ar_used + al - 1) & ~(al - 1);
    g_ar_used = at + size;
    return at;
}

/* submits the batch without waiting; it signals g_vk_csem with the next value */
static void cs_submit(void) {
    if (!g_recording) return;
    vmaFlushAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE); /* the arena may not be host coherent */
    VK_OK(vkEndCommandBuffer(g_ccb));
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_ccb};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                 .semaphore = g_vk_csem,
                                 .value = ++g_cval,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &c,
                        .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    VkResult r = vkQueueSubmit2(g_vk.compute, 1, &si, g_cfence);
    if (r == VK_ERROR_DEVICE_LOST) fatal("vulkan: the GPU device was lost (driver reset or crash)");
    g_recording = 0;
    g_inflight = VK_OK(r);
    if (!g_inflight) g_cval--; /* never signaled: draws don't wait for it, landing doesn't wait for the fence */
}
/* submits the batch if it isn't yet, waits for it, and lands its results */
static void cs_land(void) {
    if ((!g_recording && !g_inflight) || g_landing) return;
    g_landing = 1;
    u64 t0 = plat_ns();
    cs_submit();
    if (g_inflight) {
        VK_OK(vkWaitForFences(g_vk.dev, 1, &g_cfence, VK_TRUE, UINT64_MAX));
        VK_OK(vkResetFences(g_vk.dev, 1, &g_cfence));
    }
    g_vk_cs_wait_ns += plat_ns() - t0;
    vmaInvalidateAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE);
    g_inflight = 0;
    for (int i = 0; i < g_nout; i++) {
        cs_trace_output(&g_out[i]);
        vk_write_changed(g_out[i].addr, g_ar_ptr + g_out[i].off, g_org + g_out[i].off, g_out[i].size, 1);
    }
    /* only now: until the results are in memory, readers on other threads wait for this landing in
       vk_compute_mem_need */
    g_cs_any = 0;
    g_nout = 0;
    g_out_lo = ~0ull;
    g_out_hi = 0;
    g_ar_used = 0;
    vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
    if (g_ar_gfx) { /* draws read this arena: it waits until their frame is done, the next batch gets another */
        arena_retire();
        g_ar_gfx = 0;
    }
    g_vk_cs_batches++;
    g_landing = 0;
}

/* A descriptor can fail after its inputs were copied into the arena. No command references those inputs yet. */
static void cs_abandon_binding(u8* originals[8]) {
    for (int i = 0; i < 8; i++) free(originals[i]);
    if (g_recording || g_inflight) return;
    g_ar_used = 0;
    vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
}
/* a draw reads [gva, gva + len), which one pending result covers: it reads it from the arena, after the batch. Any
   other overlap lands the results now, so the caller's caches see them in memory */
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (!g_cs_any || g_landing || gva >= g_out_hi || gva + len <= g_out_lo) return 0;
    for (int i = 0; i < g_nout; i++) {
        CsOut* o = &g_out[i];
        if (gva >= o->addr + o->size || o->addr >= gva + len) continue;
        /* partly covered, or at an offset that suits no use of it (index, storage and uniform buffers): it lands */
        if (gva < o->addr || gva + len > o->addr + o->size || ((o->off + (gva - o->addr)) & (vk_stage_align() - 1))) {
            cs_land();
            return 0;
        }
        cs_submit();
        g_ar_gfx = 1;
        g_vk_cs_wait_value = g_cval;
        *buf = g_ar;
        *off = o->off + (gva - o->addr);
        return 1;
    }
    return 0;
}
void vk_compute_land(void) {
    if (!g_recording && !g_inflight) return;
    vk_lock();
    cs_land();
    vk_unlock();
}
/* the front end writes guest memory (size > 0) or reaches a point the game can observe (size 0) */
void vk_compute_before_write(u64 gva, u64 size) {
    if (!size)
        vk_compute_land();
    else if (g_cs_any)
        vk_compute_mem_need(gva, size);
}
/* a read of guest memory: results for it have to be there first. Called from any thread that reads guest memory while
   g_cs_any is set: the pending results are looked at under vk_lock, which recording and landing them hold, so a
   reader waits for a landing in progress on another thread. The landing thread itself passes (g_landing) */
void vk_compute_mem_need(u64 a, u64 n) {
    vk_lock();
    if (!g_landing && a < g_out_hi && a + n > g_out_lo)
        for (int i = 0; i < g_nout; i++)
            if (a < g_out[i].addr + g_out[i].size && g_out[i].addr < a + n) {
                cs_land();
                break;
            }
    vk_unlock();
}

/* the pipeline of a program (80 zero bytes, then the code), by its code and launch parameters, and by the constant
   buffer words its translation read */
static CsPipe* cs_pipe(const u8* code, u32 lx, u32 ly, u32 lz, u32 shm) {
    u32 blen;
    u64 key = vk_shader_hash(code, 0x10000 + 80, &blen, cs_cb_read) ^ ((u64)lx << 40) ^ ((u64)ly << 48) ^
              ((u64)lz << 56) ^ ((u64)shm << 20);
    CsPipe* p = g_csp[key & 255];
    while (p && (p->key != key || !vk_cb_reads_hold(&p->cbr, cs_cb_read))) p = p->next;
    if (p) return p;
    p = calloc(1, sizeof *p);
    p->key = key;
    p->next = g_csp[key & 255];
    g_csp[key & 255] = p;
    p->si.cs_local[0] = lx;
    p->si.cs_local[1] = ly;
    p->si.cs_local[2] = lz;
    p->si.shared_size = (int)shm;
    const char* corpus = getenv("SNX_SHADER_CORPUS");
    if (corpus) shader_corpus_save(corpus, code, blen, 0x10000 + 80, ST_CS, &p->si, key);
    size_t nw = 0;
    u32* spv = vk_translate_spirv(code, 0x10000 + 80, ST_CS, &p->si, &nw, cs_cb_read, &p->cbr);
    if (spv && !g_vk.feat.shaderFloat64) {
        for (size_t at = 5; at < nw;) {
            u32 width = spv[at] >> 16;
            if (!width || at + width > nw || (spv[at] & 0xffffu) != 17) break;
            if (width == 2 && spv[at + 1] == 10) {
                free(spv);
                spv = NULL;
                snprintf(p->si.first_unsupported, sizeof p->si.first_unsupported,
                         "shaderFloat64 is unavailable");
                break;
            }
            at += width;
        }
    }
    if (!spv || p->si.unsupported) {
        p->failed = 1;
        LOG("vulkan: compute %016llx not translated (%d instructions, first %s)\n", (unsigned long long)key,
            p->si.unsupported, p->si.first_unsupported);
        return p;
    }
    u32 image_slots = g_vk.props.limits.maxPerStageDescriptorStorageImages;
    if (image_slots > g_vk.props.limits.maxDescriptorSetStorageImages)
        image_slots = g_vk.props.limits.maxDescriptorSetStorageImages;
    if (p->si.nimg > (int)image_slots) {
        LOG_LIMIT(4, "vulkan: compute image count exceeds this GPU's descriptor limit (skipped)\n");
        free(spv);
        p->failed = 1;
        return p;
    }
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m;
    int ok = VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    free(spv);
    if (!ok) {
        p->failed = 1;
        return p;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m,
                                                .pName = "main"},
                                      .layout = g_vk_playout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &p->p))) p->failed = 1;
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (!p->failed)
        LOG_LIMIT(12, "vulkan: compute %016llx ready: block %ux%ux%u, %d global buffers, %d textures, %d images\n",
                  (unsigned long long)key, lx, ly, lz, p->si.ngbuf, p->si.ntex, p->si.nimg);
    return p;
}

/* NVN's compute shader reference points to a 0x100-byte SDK blob header. The QMD emitted by the
   guest SDK uses the raw Maxwell program at +0x100. Prepare the same CBUF view for translation,
   while the caller keeps storage results in NVN-owned buffers. */
int vk_nvn_compute_pipeline(const NvnDispatchCommand* command, VkPipeline* pipeline,
                            const ShaderInfo** info) {
    *pipeline = VK_NULL_HANDLE;
    *info = NULL;
    if (!command->uniform_updates_complete || !command->control_known || !command->program.snapshot.refs_known ||
        command->program.snapshot.shader_count != 1) return 0;
    u64 address = command->program.snapshot.refs[0].code_candidate;
    if (!address || address > UINT64_MAX - 0x100 - 0x10000) return 0;
    u32 header[2] = {0};
    gmem_read_code(address, header, sizeof header);
    if (header[0] != 0x12345679 || header[1] < 0x80 || header[1] > 0x10000) return 0;
    for (u32 bank = 0; bank < 8; bank++) g_cbn[bank] = 0;
    memset(g_cb[0], 0, 0x680);
    g_cbn[0] = 0x680;
    memcpy(g_cb[0] + 0x200, command->groups, sizeof command->groups);
    for (u32 slot = 0; slot < NVN_DRAW_STORAGE_SLOTS; slot++) {
        if (!(command->storage_bound & (1u << slot))) continue;
        const NvnBufferRange* range = &command->storage[slot];
        if (!range->address || !range->size || range->size > UINT32_MAX) return 0;
        u32 descriptor[3] = {(u32)range->address, (u32)(range->address >> 32), (u32)range->size};
        memcpy(g_cb[0] + 0x310 + slot * 16, descriptor, sizeof descriptor);
    }
    if (command->embedded_cbuf1_offset) {
        u64 offset = command->embedded_cbuf1_offset;
        if (offset < 0x100 || offset > 0x10000 || address > UINT64_MAX - offset ||
            !gmmu_translate(address + offset)) return 0;
        gmem_read_code(address + offset, g_cb[1], 256);
        g_cbn[1] = 256;
    }
    memset(g_cb[2], 0, 512);
    g_cbn[2] = 512;
    for (u32 slot = 0; slot < NVN_DRAW_TEXTURE_SLOTS; slot++)
        if (command->texture_bound & (1u << slot))
            memcpy(g_cb[2] + (8 + slot * 2) * 4, &command->texture[slot], sizeof(u32));
    for (u32 slot = 0; slot < 5; slot++) {
        if (!(command->uniform_bound & (1u << slot))) continue;
        const NvnBufferRange* range = &command->uniform[slot];
        if (!range->address || !range->size || range->size > 0x10000) return 0;
        u32 size = (u32)(range->size + 255) & ~255u;
        if (size > 0x10000) size = 0x10000;
        /* The SDK binds compute CBUFs to Maxwell at 256-byte granularity. Read that
           padded range, matching the QMD path when a shader uses its final words. */
        const NvnStorageSeed* seed = command->uniform_seed[slot];
        if (seed && seed->address == range->address && seed->size >= size)
            memcpy(g_cb[3 + slot], seed->bytes, size);
        else if (slot == 0 && command->uniform0_snapshot_size == size)
            memcpy(g_cb[3], command->uniform0_snapshot, size);
        else
            return 0;
        if (!nvn_uniform_updates_apply(command->uniform_updates, range->address,
                                       size, g_cb[3 + slot])) return 0;
        g_cbn[3 + slot] = size;
    }
    static u8 code_buf[0x10000 + 80];
    memset(code_buf, 0, 80);
    gmem_read_code(address + 0x100, code_buf + 80, header[1]);
    memset(code_buf + 80 + header[1], 0, 0x10000 - header[1]);
    CsPipe* p = cs_pipe(code_buf, command->local[0], command->local[1], command->local[2],
                        command->shared_size);
    if (!p || p->failed) return 0;
    const char* trace = getenv("SNX_NVN_DISPATCH_TRACE");
    if (trace && !strcmp(trace, "1"))
        LOG("nvn-cbuf-hash: path=direct code=%llx mask=%x b0=%llx b1=%llx b3=%llx b4=%llx\n",
            (unsigned long long)(address + 0x100), p->si.cbuf_mask,
            (unsigned long long)cs_used_hash(0, &p->si),
            (unsigned long long)cs_used_hash(1, &p->si),
            (unsigned long long)cs_used_hash(3, &p->si),
            (unsigned long long)cs_used_hash(4, &p->si));
    *pipeline = p->p;
    *info = &p->si;
    return 1;
}

const u8* vk_nvn_compute_cbuf(u32 bank, u32* size) {
    *size = bank < 8 ? g_cbn[bank] : 0;
    return bank < 8 ? g_cb[bank] : NULL;
}

void vk_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    u32* rc = ch->rcomp;
    u32 gx = q[12] & 0x7FFFFFFF, gy = q[13] & 0xFFFF, gz = q[13] >> 16, lx = q[18] >> 16, ly = q[19] & 0xFFFF,
        lz = q[19] >> 16, shm = q[17] & 0x3FFFF;
    if (!gx || !gy || !gz || !lx || !ly || !lz) return;
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    if (gx > lim->maxComputeWorkGroupCount[0] || gy > lim->maxComputeWorkGroupCount[1] ||
        gz > lim->maxComputeWorkGroupCount[2] || lx > lim->maxComputeWorkGroupSize[0] ||
        ly > lim->maxComputeWorkGroupSize[1] || lz > lim->maxComputeWorkGroupSize[2] ||
        (u64)lx * ly * lz > lim->maxComputeWorkGroupInvocations || shm > lim->maxComputeSharedMemorySize) {
        LOG_LIMIT(1,
                  "vulkan: a dispatch of %ux%ux%u blocks of %ux%ux%u with %u bytes shared is past this GPU's limits "
                  "(skipped)\n",
                  gx, gy, gz, lx, ly, lz, shm);
        g_vk_cs_skipped++;
        return;
    }
    cs_init();
    /* constant buffers from the QMD (reading them lands results they overlap) */
    u64 cba[8] = {0};
    for (int i = 0; i < 8; i++) {
        g_cbn[i] = 0;
        if (!((q[20] >> i) & 1)) continue;
        cba[i] = ((u64)(q[30 + 2 * i] & 0xFF) << 32) | q[29 + 2 * i];
        u32 sz = (q[30 + 2 * i] >> 15) & 0x1FFFF;
        if (!sz || sz > 0x10000) sz = 0x10000;
        g_cbn[i] = sz;
        gmem_read(cba[i], g_cb[i], sz);
    }
    /* the program at an address is taken as the same until the next submit, as vk_shader_get does: the code isn't
       read and hashed again for every dispatch of it */
    static struct {
        u64 addr;
        u32 epoch, lx, ly, lz, shm;
        CsPipe* p;
    } fc[64];
    u64 pa = code + q[8];
    typeof(fc[0])* fe = &fc[((pa >> 8) ^ (pa >> 16)) & 63];
    CsPipe* p = fe->p;
    if (!p || fe->addr != pa || fe->epoch != g_vk_epoch || fe->lx != lx || fe->ly != ly || fe->lz != lz ||
        fe->shm != shm || !vk_cb_reads_hold(&p->cbr, cs_cb_read)) {
        static u8 code_buf[0x10000 + 80]; /* compute programs have no SPH: 80 zero bytes stand in for one */
        u32 code_size = 0x10000;
        /* NVN places a size header immediately before its code. Adjacent programs can otherwise
           look like a live tail to an unresolved BRX scan. Other QMD clients keep the full window. */
        if (pa >= 0x100) {
            const u8* header = gmem_ptr_untracked(pa - 0x100, 8);
            if (header) {
                u32 magic, declared;
                memcpy(&magic, header, sizeof magic);
                memcpy(&declared, header + 4, sizeof declared);
                if (magic == 0x12345679 && declared >= 0x80 && declared <= 0x10000 && !(declared & 7) &&
                    gmem_ptr_untracked(pa, declared))
                    code_size = declared;
            }
        }
        memset(code_buf, 0, 80);
        gmem_read_code(pa, code_buf + 80, code_size);
        memset(code_buf + 80 + code_size, 0, 0x10000 - code_size);
        p = cs_pipe(code_buf, lx, ly, lz, shm);
        *fe = (typeof(fc[0])){pa, g_vk_epoch, lx, ly, lz, shm, p};
    }
    if (p->failed) {
        g_vk_cs_skipped++;
        return;
    }
    const char* cbuf_trace = getenv("SNX_NVN_DISPATCH_TRACE");
    if (cbuf_trace && !strcmp(cbuf_trace, "1"))
        LOG("nvn-cbuf-hash: path=qmd code=%llx mask=%x b0=%llx b1=%llx b3=%llx b4=%llx\n",
            (unsigned long long)pa, p->si.cbuf_mask,
            (unsigned long long)cs_used_hash(0, &p->si),
            (unsigned long long)cs_used_hash(1, &p->si),
            (unsigned long long)cs_used_hash(3, &p->si),
            (unsigned long long)cs_used_hash(4, &p->si));
    int needs_unformatted_image = 0;
    for (int i = 0; i < p->si.nimg; i++) needs_unformatted_image |= p->si.img[i].kind == 0;
    if (needs_unformatted_image && !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        LOG_LIMIT(1, "vulkan: storage image writes need shaderStorageImageWriteWithoutFormat (skipped)\n");
        g_vk_cs_skipped++;
        return;
    }
    /* storage buffers: {address, size} in constant buffer 0 */
    u64 sa[16];
    u32 ss[16];
    int ng = p->si.ngbuf > 16 ? 16 : p->si.ngbuf;
    u64 need = 0;
    for (int i = 0; i < ng; i++) {
        u32 v[3] = {0, 0, 0};
        cs_cb_read(0, p->si.gbuf_cbuf_off[i], &v[0]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 4, &v[1]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 8, &v[2]);
        sa[i] = ((u64)v[1] << 32) | v[0];
        ss[i] = v[2] ? v[2] : 16;
        if (ss[i] > (16u << 20)) ss[i] = 16u << 20;
        ss[i] = (ss[i] + 3) & ~3u;
        need += ss[i] + vk_stage_align();
    }
    const char* dispatch_trace = getenv("SNX_NVN_DISPATCH_TRACE");
    if (dispatch_trace && !strcmp(dispatch_trace, "1"))
        LOG("nvn-qmd-dispatch: code=%llx groups=%ux%ux%u local=%ux%ux%u shared=%u buffers=%d writes=%x textures=%d cbuf=%x desc0=%x desc1=%x g0=%llx/%u g1=%llx/%u cb1=%llx/%u cb2=%llx/%u cb3=%llx/%u cb4=%llx/%u cb0_200=%08x,%08x,%08x cb0_640=%08x\n",
            (unsigned long long)pa, gx, gy, gz, lx, ly, lz, shm, ng, p->si.gbuf_wmask,
            p->si.ntex, p->si.cbuf_mask, ng > 0 ? p->si.gbuf_cbuf_off[0] : 0,
            ng > 1 ? p->si.gbuf_cbuf_off[1] : 0,
            (unsigned long long)(ng > 0 ? sa[0] : 0), ng > 0 ? ss[0] : 0,
            (unsigned long long)(ng > 1 ? sa[1] : 0), ng > 1 ? ss[1] : 0,
            (unsigned long long)cba[1], g_cbn[1], (unsigned long long)cba[2], g_cbn[2],
            (unsigned long long)cba[3], g_cbn[3], (unsigned long long)cba[4], g_cbn[4],
            g_cbn[0] >= 0x204 ? *(const u32*)(g_cb[0] + 0x200) : 0,
            g_cbn[0] >= 0x208 ? *(const u32*)(g_cb[0] + 0x204) : 0,
            g_cbn[0] >= 0x20c ? *(const u32*)(g_cb[0] + 0x208) : 0,
            g_cbn[0] >= 0x644 ? *(const u32*)(g_cb[0] + 0x640) : 0);
    if (dispatch_trace && !strcmp(dispatch_trace, "1") && p->si.ntex) {
        const ShTex* t = &p->si.tex[0];
        u32 h = 0, h2 = 0, bank = t->bindless ? t->bl_cbuf : (rc[0x982] & 7);
        cs_cb_read(bank, t->cbuf_off * 4, &h);
        if (t->bl2 && cs_cb_read(t->bl2_cbuf, t->bl2_off * 4, &h2)) h |= h2;
        LOG("nvn-qmd-texture: code=%llx bank=%u word=%x handle=%08x type=%u array=%u shadow=%u bindless=%u bl2=%u second=%u/%x\n",
            (unsigned long long)pa, bank, t->cbuf_off, h, t->type, t->array, t->shadow,
            t->bindless, t->bl2, t->bl2_cbuf, t->bl2_off);
    }
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) need += g_cbn[i] + vk_stage_align();
    need += sizeof(i32) * 320 + vk_stage_align(); /* optional format data */
    /* dispatches that read textures run on the graphics queue, after what was recorded before them. Inputs that
       pending results overlap land those first, before anything is put in the arena */
    int gfx = p->si.ntex > 0 || p->si.nimg > 0, overlap = 0;
    /* a read-only input inside a pending result of this batch is read from the arena on the GPU, after a barrier
       (multi-pass skinning reads what the pass before wrote); any other overlap lands the results first */
    i64 chain[16];
    for (int i = 0; i < ng; i++) {
        chain[i] = -1;
        for (int k = 0; k < g_nout && !overlap; k++) {
            CsOut* o = &g_out[k];
            if (sa[i] >= o->addr + o->size || o->addr >= sa[i] + ss[i]) continue;
            if (!((p->si.gbuf_wmask >> i) & 1) && g_recording && sa[i] >= o->addr && sa[i] + ss[i] <= o->addr + o->size)
                chain[i] = (i64)(o->off + (sa[i] - o->addr));
            else
                overlap = 1;
        }
    }
    if (g_recording && g_ar_used + need > g_ar_cap && g_ar_want < (64ull << 20))
        g_ar_want *= 2; /* fewer, bigger batches */
    if (gfx || overlap || g_inflight || g_nout + ng > 256 || g_ar_used + need > g_ar_cap) {
        cs_land();
        for (int i = 0; i < ng; i++) chain[i] = -1; /* the results are in memory now */
    }
    if (!g_recording && !arena_fit(need)) return;

    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = g_dpool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &g_vk_dslayout[shader_vk_set(ST_CS)]};
    if (vkAllocateDescriptorSets(g_vk.dev, &dai, &set) != VK_SUCCESS) {
        cs_land();
        if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &dai, &set))) return;
    }
    VkWriteDescriptorSet w[80];
    VkDescriptorBufferInfo bi[80];
    VkDescriptorImageInfo ii[40];
    Surf* images[8] = {0};
    u8* originals[8] = {0};
    u32 nw = 0;
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) {
            /* only the bytes the shader reads (robust buffer access keeps reads past them in bounds, of undefined
               value) */
            u32 range = g_cbn[i];
            if (p->si.cbuf_max[i] && range > p->si.cbuf_max[i]) range = p->si.cbuf_max[i];
            range = (range + 15) & ~15u;
            if (!range) range = 16;
            if (range > g_vk.props.limits.maxUniformBufferRange) range = g_vk.props.limits.maxUniformBufferRange;
            u64 o = arena_alloc(range);
            u32 n = g_cbn[i] < range ? g_cbn[i] : range;
            memcpy(g_ar_ptr + o, g_cb[i], n);
            memset(g_ar_ptr + o + n, 0, range - n);
            bi[nw] = (VkDescriptorBufferInfo){g_ar, o, range};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = SNX_VKB_CB + i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &bi[nw]};
            nw++;
        }
    u64 so[16];
    int chained = 0;
    for (int i = 0; i < ng; i++) {
        if (chain[i] >= 0) chained = 1;
        u64 o = so[i] = chain[i] >= 0 ? (u64)chain[i] : arena_alloc(ss[i]);
        if (chain[i] >= 0)
            ; /* written by an earlier dispatch of the batch, read in place */
        else if (sa[i])
            gmem_read(sa[i], g_ar_ptr + o, ss[i]);
        else
            memset(g_ar_ptr + o, 0, ss[i]);
        const char* input_trace = getenv("SNX_NVN_SHADOW_INPUT_TRACE");
        /* A chained input lives in a GPU-written arena region, so its host copy is not an oracle yet. */
        if (input_trace && !strcmp(input_trace, "1") && chain[i] < 0 && (p->si.gbuf_wmask & (1u << i)))
            cs_shadow_expect(sa[i], ss[i], pa, g_ar_ptr + o, 1);
        if (ss[i] == 58848 && input_trace && !strcmp(input_trace, "1"))
            LOG_LIMIT(8, "nvn-qmd-storage-input: code=%llx addr=%llx hash=%llx byte304=%02x\n",
                      (unsigned long long)pa, (unsigned long long)sa[i],
                      (unsigned long long)cs_probe_hash(g_ar_ptr + o, ss[i]), g_ar_ptr[o + 304]);
        bi[nw] = (VkDescriptorBufferInfo){g_ar, o, ss[i]};
        w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = SNX_VKB_SSBO + i,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &bi[nw]};
        nw++;
    }
    VkCommandBuffer cb;
    if (gfx) {
        /* textures from the compute engine's headers; texture handles from its texture constant buffer bank. Compute
           shaders declare every texture as float (they aren't keyed on integer textures), and depth textures aren't
           sampled here */
        u64 tic_base = ((u64)rc[0x55D] << 32) | rc[0x55E], tsc_base = ((u64)rc[0x557] << 32) | rc[0x558];
        u32 tcb = rc[0x982] & 7;
        int fmt_used = 0;
        i32 fmt[320] = {0}; /* SNX_FMT, std140: snx_ic[8] snx_tc[32] snx_ib[8] snx_tb[32], 16 bytes each */
        for (int i = 0; i < p->si.ntex && i < SH_MAXTEX; i++) {
            ShTex* st = &p->si.tex[i];
            u32 h = 0, h2 = 0;
            cs_cb_read(st->bindless ? (st->bl_cbuf & 7) : tcb, st->cbuf_off * 4, &h);
            if (st->bindless && st->bl2 && cs_cb_read(st->bl2_cbuf & 7, st->bl2_off * 4, &h2)) h |= h2;
            u32 tic[8], tsc[8];
            gmem_read(tic_base + (h & 0xFFFFF) * 32ull, tic, 32);
            gmem_read(tsc_base + ((h >> 20) & 0xFFF) * 32ull, tsc, 32);
            VkImageViewType want = vk_tex_view_type(st);
            if (st->flags & 2) {
                LOG_LIMIT(4, "vulkan: multisample texture upload is unavailable (skipped)\n");
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            Surf* t = st->flags & 1 ? vk_tex_from_tic_raw(tic) : vk_tex_from_tic(tic);
            if ((st->flags & 1) && !t) {
                LOG_LIMIT(4, "vulkan: raw surface load requires an RGBA8 texture (skipped)\n");
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            VkImageView v = t && !t->f.depth ? vk_tex_bind_view(t, want, tic, st->flags & 1 ? 1 : 0)
                                               : VK_NULL_HANDLE;
            VkImageLayout sampled_layout =
                p->si.nimg ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (v)
                vk_surf_use(vk_cmd(), t, sampled_layout, VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            else
                v = vk_null_view(want, 0);
            VkSampler smp = vk_tex_bind_sampler(tsc, st, st->flags & 1 ? 1 : 0);
            if (!v || !smp) { /* before any output is recorded: nothing of this dispatch lands */
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            ii[i] = (VkDescriptorImageInfo){smp, v, sampled_layout};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = SNX_VKB_TEX + i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &ii[i]};
            fmt[(8 + i) * 4] = t ? (t->f.vk == VK_FORMAT_R16G16B16A16_SFLOAT || t->f.vk == VK_FORMAT_R16G16_SFLOAT ||
                                            t->f.vk == VK_FORMAT_R16_SFLOAT
                                        ? 1
                                    : t->f.bpp == 4 && !t->f.is_int ? 2
                                                                    : 0)
                                 : 0;
            fmt[(48 + i) * 4] = t && t->f.bpp ? t->f.bpp : 4;
            fmt_used = 1;
        }
        for (int i = 0; i < p->si.nimg; i++) {
            const u32 off = p->si.img[i].cbuf_off;
            u32 h = 0, tic[8];
            if (!cs_cb_read(p->si.img[i].bindless ? (p->si.img[i].bl_cbuf & 7) : tcb, off * 4, &h)) {
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            gmem_read(tic_base + (h & 0xFFFFF) * 32ull, tic, sizeof tic);
            Surf* t = vk_tex_from_tic(tic);
            u64 image_bytes = vk_surf_guest_size(t);
            VkComponentMapping identity = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            VkImageView view = t && vk_surf_can_writeback(t) && t->storage_capable && t->f.raw && !t->f.depth &&
                                       (p->si.img[i].kind ? t->f.vk == VK_FORMAT_R32_UINT : !t->f.is_int) &&
                                       t->levels == 1 && t->depth3d == 1 && p->si.img[i].type == 3 && image_bytes &&
                                       image_bytes <= UINT32_MAX
                                   ? vk_tex_view(t, VK_IMAGE_VIEW_TYPE_2D, 0, &identity)
                                   : VK_NULL_HANDLE;
            if (!view) {
                LOG_LIMIT(4, "vulkan: unsupported compute storage image (skipped)\n");
                g_vk_cs_skipped++;
                cs_abandon_binding(originals);
                return;
            }
            int seen = 0;
            for (int k = 0; k < i; k++)
                if (images[k] == t) {
                    seen = 1;
                    break;
                }
            if (!seen) {
                originals[i] = malloc(image_bytes);
                if (!originals[i]) {
                    g_vk_cs_skipped++;
                    cs_abandon_binding(originals);
                    return;
                }
                gmem_read(t->gva, originals[i], image_bytes);
            }
            vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            images[i] = t;
            ii[p->si.ntex + i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = SNX_VKB_IMG + i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                             .pImageInfo = &ii[p->si.ntex + i]};
        }
        if (fmt_used) {
            u64 o = arena_alloc(sizeof fmt);
            memcpy(g_ar_ptr + o, fmt, sizeof fmt);
            bi[nw] = (VkDescriptorBufferInfo){g_ar, o, sizeof fmt};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = SNX_VKB_FMT,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &bi[nw]};
            nw++;
        }
    }
    /* the outputs, once every input is read (an input can alias another storage buffer's output) */
    for (int i = 0; i < ng; i++)
        if (((p->si.gbuf_wmask >> i) & 1) && sa[i]) {
            memcpy(g_org + so[i], g_ar_ptr + so[i], ss[i]);
            g_out[g_nout++] = (CsOut){sa[i], so[i], ss[i], pa};
            if (sa[i] < g_out_lo) g_out_lo = sa[i];
            if (sa[i] + ss[i] > g_out_hi) g_out_hi = sa[i] + ss[i];
        }
    if (gfx) {
        vk_render_end();
        cb = vk_cmd();
    } else {
        if (!g_recording) {
            VkCommandBufferBeginInfo b = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VK_OK(vkBeginCommandBuffer(g_ccb, &b));
            g_recording = 1;
        }
        cb = g_ccb;
    }
    vkUpdateDescriptorSets(g_vk.dev, nw, w, 0, NULL);
    if (chained) {
        VkMemoryBarrier2 cmb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        VkDependencyInfo cdi = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &cmb};
        vkCmdPipelineBarrier2(cb, &cdi);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk_playout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    /* results visible to the host */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    g_vk_dispatches++;
    if (gfx) {
        /* everything before it on the graphics queue runs first; then its results land at once */
        vmaFlushAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE); /* the arena may not be host coherent */
        vk_finish();
        vmaInvalidateAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE);
        g_landing = 1;
        for (int i = 0; i < g_nout; i++) {
            cs_trace_output(&g_out[i]);
            vk_write_changed(g_out[i].addr, g_ar_ptr + g_out[i].off, g_org + g_out[i].off, g_out[i].size, 1);
        }
        for (int i = 0; i < p->si.nimg; i++) {
            int seen = 0;
            for (int k = 0; k < i; k++)
                if (images[k] == images[i]) {
                    seen = 1;
                    break;
                }
            if (seen) continue;
            images[i]->wgen++;
            vk_surf_flush(images[i], originals[i]);
        }
        for (int i = 0; i < p->si.nimg; i++) free(originals[i]);
        g_nout = 0;
        g_out_lo = ~0ull;
        g_out_hi = 0;
        g_ar_used = 0;
        vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
        g_landing = 0;
        return;
    }
    g_cs_any = g_nout > 0;
}
