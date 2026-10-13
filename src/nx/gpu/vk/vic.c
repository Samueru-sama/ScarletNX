/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The VIC composition as a compute shader (gpu/video/vic.comp). gpu/video/vic.c hands over the job's planes and its
   slots' parameters; the planes travel into a host-visible buffer byte for byte as they lie in guest memory (the
   shader's address math matches the CPU's, bl_offset included), the shader composes the picture, and the output
   planes travel back the same way. The call is synchronous: when it returns the picture is in guest memory, which
   is what the CPU path in gpu/video/vic.c gives the callers too.

   The output planes are copied in as well as out: the shader writes whole words, one owning invocation each, and
   the bytes it does not own (outside the slot rectangles, past the width) keep their guest values. */
#include <stdlib.h>
#include <string.h>
#include "gpu/renderer.h"
#include "gpu/vk/vk.h"
#include "gpu/video/vic_spv.h"
#include "runtime/platform/log.h"

/* the shader's structs (gpu/video/vic.comp): only 32 bit members, so std430 packs them exactly as C does */
typedef struct {
    u32 offset, pitch, height, lim, blk, bh;
} GpuPlane;
typedef struct {
    u32 on;
    u32 sx0, sy0, dx0, dy0, w, h;
    int mat[12]; /* the color matrix rows: c00 c01 c02 c03 c10 ... c23 */
    u32 mat_shift, mat_enable;
    u32 alpha, clamp_lo, clamp_hi;
    GpuPlane iy, ic;
} GpuSlot;
typedef struct {
    u32 fmt, sw, sh, nslots;
    GpuPlane ol, oc;
    GpuSlot slots[8];
} GpuParams;
_Static_assert(sizeof(GpuPlane) == 24, "GpuPlane disagrees with the shader");
_Static_assert(sizeof(GpuSlot) == 144, "GpuSlot disagrees with the shader");
_Static_assert(sizeof(GpuParams) == 64 + 8 * 144, "GpuParams disagrees with the shader");

static VkDescriptorSetLayout g_vic_ds;
static VkPipelineLayout g_vic_layout;
static VkPipeline g_vic_pipe, g_vic_pipe_unaligned;
static VkBuffer g_vic_buf;
static VmaAllocation g_vic_mem;
static u8* g_vic_ptr;
static u64 g_vic_cap;
static VkCommandPool g_vic_cpool;
static VkCommandBuffer g_vic_cb;
static VkFence g_vic_fence;

static void vic_init(void) {
    if (g_vic_pipe) return;
    VkDescriptorSetLayoutBinding bindings[4];
    for (u32 i = 0; i < 4; i++)
        bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                     VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = bindings};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_vic_ds)))
        fatal("vulkan: cannot create the VIC descriptor layout");
    VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_vic_ds};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_vic_layout)))
        fatal("vulkan: cannot create the VIC pipeline layout");
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vic_spv, .pCode = vic_spv};
    VkShaderModuleCreateInfo mi_unaligned = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                             .codeSize = sizeof vic_unaligned_spv,
                                             .pCode = vic_unaligned_spv};
    /* two builds of the shader: the aligned one is the happy path, the unaligned one handles planes whose rows
       are not word aligned (gpu/video/vic.comp) */
    VkPipeline* pipes[2] = {&g_vic_pipe, &g_vic_pipe_unaligned};
    const VkShaderModuleCreateInfo* mis[2] = {&mi, &mi_unaligned};
    for (int i = 0; i < 2; i++) {
        VkShaderModule module;
        if (!VK_OK(vkCreateShaderModule(g_vk.dev, mis[i], NULL, &module)))
            fatal("vulkan: cannot load the VIC shader");
        VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = module,
                                                    .pName = "main"},
                                          .layout = g_vic_layout};
        int ok = VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, pipes[i]));
        vkDestroyShaderModule(g_vk.dev, module, NULL);
        if (!ok) fatal("vulkan: cannot create the VIC compute pipeline");
    }
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_vk.compute_family};
    if (!VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_vic_cpool)))
        fatal("vulkan: cannot create the VIC command pool");
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_vic_cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    if (!VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_vic_cb)))
        fatal("vulkan: cannot allocate the VIC command buffer");
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!VK_OK(vkCreateFence(g_vk.dev, &fci, NULL, &g_vic_fence))) fatal("vulkan: cannot create the VIC fence");
}

/* one buffer for the job, grown as needed: every job waits for its fence before it returns, so the old buffer is
   idle when the next job replaces it */
static int buf_fit(u64 need) {
    if (g_vic_buf && need <= g_vic_cap) return 1;
    if (g_vic_buf) {
        vmaDestroyBuffer(g_vk.vma, g_vic_buf, g_vic_mem);
        g_vic_buf = VK_NULL_HANDLE;
        g_vic_cap = 0;
    }
    u64 cap = 8ull << 20;
    while (cap < need) cap *= 2;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = cap,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_vic_buf, &g_vic_mem, &info))) {
        g_vic_buf = VK_NULL_HANDLE;
        return 0;
    }
    g_vic_ptr = info.pMappedData;
    g_vic_cap = cap;
    return 1;
}

static GpuPlane plane_desc(const VicPlane* p, u32 offset) {
    /* an unmapped plane composes nothing: its limits are zero in the shader */
    if (!p->p) return (GpuPlane){offset, 0, 0, 0, 0, 0};
    return (GpuPlane){offset, p->pitch, p->h, (u32)p->lim, p->blk, p->bh};
}

/* the aligned shader build needs every plane's rows to keep the word aligned columns word aligned: block linear
   rows do (bl_offset keeps the column's alignment), pitch linear rows only when the pitch is a multiple of 4 */
static int job_aligned(const VicJob* j) {
    for (int s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        if (!sl->on) continue;
        if (sl->iy.p && !sl->iy.blk && (sl->iy.pitch & 3)) return 0;
        if (sl->ic.p && !sl->ic.blk && (sl->ic.pitch & 3)) return 0;
    }
    if (j->oy.p && !j->oy.blk && (j->oy.pitch & 3)) return 0;
    if (j->oc.p && !j->oc.blk && (j->oc.pitch & 3)) return 0;
    return 1;
}

int vk_vic_compose(const VicJob* j) {
    vic_init();
    u64 al = vk_stage_align();
    /* the buffer's layout: the parameters, every slot's input planes, then the two output planes */
    u64 params_off = 0, in_off = params_off + sizeof(GpuParams), at = in_off;
    u32 in_offs[8][2] = {{0}};
    for (int s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        if (!sl->on) continue;
        u64 lims[2] = {sl->iy.lim, sl->ic.lim};
        for (int k = 0; k < 2; k++) {
            at = (at + al - 1) & ~(al - 1);
            in_offs[s][k] = (u32)at;
            at += lims[k];
        }
    }
    u64 ol_off = (at + al - 1) & ~(al - 1), ol_size = (j->oy.lim + 3) & ~3ull;
    /* every binding offset, the chroma's too, is storage aligned */
    u64 oc_off = (ol_off + ol_size + al - 1) & ~(al - 1), oc_size = (j->oc.lim + 3) & ~3ull;
    if (oc_size < 4) oc_size = 4;
    u64 total = oc_off + oc_size;
    u64 range_max = g_vk.props.limits.maxStorageBufferRange;
    if (total > range_max || ol_size > range_max || oc_size > range_max || at > range_max) {
        LOG_LIMIT(4, "video: VIC job too large for the GPU (%llu bytes), composed in guest memory\n",
                  (unsigned long long)total);
        return 0;
    }
    if (!buf_fit(total)) {
        LOG_LIMIT(4, "video: no GPU memory for a VIC job, composed in guest memory\n");
        return 0;
    }

    GpuParams p = {0};
    p.fmt = j->fmt;
    p.sw = j->sw;
    p.sh = j->sh;
    p.nslots = 8;
    p.ol = plane_desc(&j->oy, 0);
    p.oc = plane_desc(&j->oc, 0);
    for (int s = 0; s < 8; s++) {
        const VicSlot* sl = &j->slots[s];
        GpuSlot* g = &p.slots[s];
        g->on = (u32)sl->on;
        g->sx0 = sl->sx0;
        g->sy0 = sl->sy0;
        g->dx0 = sl->dx0;
        g->dy0 = sl->dy0;
        g->w = sl->w;
        g->h = sl->h;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) g->mat[r * 4 + c] = sl->mat[r][c];
        g->mat_shift = (u32)sl->mat_shift;
        g->mat_enable = (u32)sl->mat_enable;
        g->alpha = sl->alpha;
        g->clamp_lo = sl->clamp_lo;
        g->clamp_hi = sl->clamp_hi;
        if (!sl->on) continue;
        g->iy = plane_desc(&sl->iy, in_offs[s][0]);
        g->ic = plane_desc(&sl->ic, in_offs[s][1]);
        memcpy(g_vic_ptr + in_offs[s][0], sl->iy.p, sl->iy.lim);
        memcpy(g_vic_ptr + in_offs[s][1], sl->ic.p, sl->ic.lim);
    }
    memcpy(g_vic_ptr + params_off, &p, sizeof p);
    /* the output planes round trip: the shader owns whole words and leaves the other bytes alone */
    memcpy(g_vic_ptr + ol_off, j->oy.p, j->oy.lim);
    if (j->oc.p) memcpy(g_vic_ptr + oc_off, j->oc.p, j->oc.lim);
    vmaFlushAllocation(g_vk.vma, g_vic_mem, 0, VK_WHOLE_SIZE);

    VkDescriptorSet set = vk_desc_alloc(g_vic_ds);
    if (!set) {
        LOG_LIMIT(4, "video: no GPU descriptors for a VIC job, composed in guest memory\n");
        return 0;
    }
    VkDescriptorBufferInfo buffers[4] = {
        {g_vic_buf, params_off, sizeof(GpuParams)},
        /* from 0: in_offs count from the start of the buffer */
        {g_vic_buf, 0, at},
        {g_vic_buf, ol_off, ol_size},
        {g_vic_buf, oc_off, oc_size}};
    VkWriteDescriptorSet writes[4];
    for (u32 i = 0; i < 4; i++)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(g_vk.dev, 4, writes, 0, NULL);

    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(g_vic_cb, &bi));
    vkCmdBindPipeline(g_vic_cb, VK_PIPELINE_BIND_POINT_COMPUTE, job_aligned(j) ? g_vic_pipe : g_vic_pipe_unaligned);
    vkCmdBindDescriptorSets(g_vic_cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vic_layout, 0, 1, &set, 0, NULL);
    /* a workgroup composes a 32x8 tile of output pixels (gpu/video/vic.comp) */
    vkCmdDispatch(g_vic_cb, (j->sw + 31) / 32, (j->sh + 7) / 8, 1);
    VK_OK(vkEndCommandBuffer(g_vic_cb));
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_vic_cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &c};
    VkResult r = vkQueueSubmit2(g_vk.compute, 1, &si, g_vic_fence);
    if (r == VK_ERROR_DEVICE_LOST) fatal("vulkan: the GPU device was lost (driver reset or crash)");
    if (r != VK_SUCCESS) return 0;
    VK_OK(vkWaitForFences(g_vk.dev, 1, &g_vic_fence, VK_TRUE, UINT64_MAX));
    VK_OK(vkResetFences(g_vk.dev, 1, &g_vic_fence));
    vmaInvalidateAllocation(g_vk.vma, g_vic_mem, 0, VK_WHOLE_SIZE);
    memcpy(j->oy.p, g_vic_ptr + ol_off, j->oy.lim);
    if (j->oc.p) memcpy(j->oc.p, g_vic_ptr + oc_off, j->oc.lim);
    return 1;
}
