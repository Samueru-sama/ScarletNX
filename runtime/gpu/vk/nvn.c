/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* First NVN-owned render images. They are deliberately separate from guest Maxwell surfaces until whole-list
   replacement and guest-memory coherence are implemented. Only complete, full 2D clears are accepted. */
#include "gpu/vk/vk.h"
#include "platform/log.h"
#include <string.h>

#define NVN_IMAGE_SLOTS 16
typedef struct {
    u64 texture, pool, address, texture_generation, pool_generation;
    u32 width, height, format;
    VkImage image;
    VmaAllocation allocation;
    VkImageLayout layout;
} NvnImage;
static NvnImage images[NVN_IMAGE_SLOTS];

static VkFmt nvn_format(u32 format) {
    switch (format) {
        case 0x0a: return (VkFmt){.vk = VK_FORMAT_R32_SFLOAT, .bpp = 4};          /* R32F */
        case 0x25: return vk_rt_format(0xd5);                                     /* RGBA8 */
        case 0x29: return (VkFmt){.vk = VK_FORMAT_R16G16B16A16_SFLOAT, .bpp = 8}; /* RGBA16F */
        case 0x38: return (VkFmt){.vk = VK_FORMAT_R8G8B8A8_SRGB, .bpp = 4};       /* RGBA8 SRGB */
        case 0x3f: return vk_rt_format(0xe0);                                     /* R11G11B10F */
        case 0x32: return vk_zt_format(0x13);                                     /* D16 */
        case 0x34: return vk_zt_format(0x0a);                                     /* D32F */
        case 0x35: return vk_zt_format(0x14);                                     /* D24S8 */
        default: return (VkFmt){0};
    }
}

static NvnImage* image_for(const NvnTextureTarget* target, VkFormat format, VkImageAspectFlags aspect) {
    NvnImage* empty = NULL;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        NvnImage* image = &images[i];
        if (!image->image) {
            if (!empty) empty = image;
            continue;
        }
        if (image->texture == target->texture && image->pool == target->pool && image->address == target->gpu_address &&
            image->texture_generation == target->texture_generation &&
            image->pool_generation == target->pool_generation && image->width == target->width &&
            image->height == target->height && image->format == target->format)
            return image;
    }
    if (!empty) return NULL;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {target->width, target->height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage =
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  (aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                                                      : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)};
    VmaAllocationCreateInfo allocation = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = NULL;
    if (vmaCreateImage(g_vk.vma, &info, &allocation, &image, &memory, NULL) != VK_SUCCESS) return NULL;
    empty->image = image;
    empty->allocation = memory;
    empty->texture = target->texture;
    empty->pool = target->pool;
    empty->address = target->gpu_address;
    empty->texture_generation = target->texture_generation;
    empty->pool_generation = target->pool_generation;
    empty->width = target->width;
    empty->height = target->height;
    empty->format = target->format;
    empty->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return empty;
}

int vk_nvn_clear(const NvnClearCommand* command) {
    const NvnTextureTarget* target = &command->target;
    if (target->target != 1 || !target->width || !target->height || target->depth != 1 || target->levels != 1 ||
        target->samples > 1 || target->view.base_level || target->view.first_layer || (target->view.level_count > 1) ||
        (target->view.layer_count > 1) || (target->view.flags & (8 | 16)))
        return 0;
    VkFmt format = nvn_format(target->format);
    if (!format.vk) return 0;
    if ((target->format == 0x25 && format.vk != VK_FORMAT_R8G8B8A8_UNORM) ||
        (target->format == 0x3f && format.vk != VK_FORMAT_B10G11R11_UFLOAT_PACK32))
        return 0; /* vk_rt_format may substitute RGBA8 on unsupported devices */
    if (command->kind == NVN_CLEAR_COLOR) {
        if (format.depth || command->mask != 15) return 0;
    } else if (command->kind == NVN_CLEAR_DEPTH_STENCIL) {
        if (!format.depth ||
            (command->stencil_mask && command->stencil_mask != 0xff && command->stencil_mask != 0xffffffffu) ||
            (!command->depth_mask && !command->stencil_mask) || (command->stencil_mask && !format.stencil))
            return 0;
    } else {
        return 0;
    }
    VkImageAspectFlags aspect = vk_aspect(&format);
    NvnImage* image = image_for(target, format.vk, aspect);
    if (!image) return 0;
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_barrier(cb, image->image, aspect,
               image->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                                          : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               image->layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_TRANSFER_WRITE_BIT, image->layout,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageSubresourceRange range = {aspect, 0, 1, 0, 1};
    if (command->kind == NVN_CLEAR_COLOR) {
        VkClearColorValue color = {0};
        memcpy(color.float32, command->color, sizeof command->color);
        vkCmdClearColorImage(cb, image->image, image->layout, &color, 1, &range);
    } else {
        range.aspectMask = (command->depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                           (command->stencil_mask ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        VkClearDepthStencilValue value = {.stencil = command->stencil};
        memcpy(&value.depth, &command->depth_bits, sizeof value.depth);
        vkCmdClearDepthStencilImage(cb, image->image, image->layout, &value, 1, &range);
    }
    return 1;
}

/* Prepare exact guest buffer snapshots on the GPU thread. These are inputs for a future
   host draw; the guest GPFIFO remains responsible for rendering and presentation. */
int vk_nvn_draw_prepare(const NvnDrawCommand* command) {
    enum { MAX_RANGE = 1 << 20, MAX_TOTAL = 4 << 20 };
    u64 total = 0;
    for (u32 i = 0; i < NVN_DRAW_VERTEX_SLOTS; i++) {
        if (!(command->vertex_bound & (1u << i))) continue;
        const NvnBufferRange* range = &command->vertex[i];
        if (!range->address || !range->size || range->size > MAX_RANGE || total + range->size > MAX_TOTAL) return 0;
        total += range->size;
    }
    for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES; stage++)
        for (u32 i = 0; i < NVN_DRAW_UNIFORM_SLOTS; i++) {
            if (!(command->uniform_bound[stage] & (1u << i))) continue;
            const NvnBufferRange* range = &command->uniform[stage][i];
            if (!range->address || !range->size || range->size > MAX_RANGE || total + range->size > MAX_TOTAL) return 0;
            total += range->size;
        }
    u64 index_bytes = 0;
    if (command->indexed) {
        if (command->index_type > 2 || !command->index_address) return 0;
        index_bytes = (u64)command->count << command->index_type;
        if (!index_bytes || index_bytes > MAX_RANGE || total + index_bytes > MAX_TOTAL) return 0;
    }
    VkBuffer buffer;
    u64 offset;
    for (u32 i = 0; i < NVN_DRAW_VERTEX_SLOTS; i++)
        if ((command->vertex_bound & (1u << i)) &&
            !vk_buf_get_strict(command->vertex[i].address, command->vertex[i].size, &buffer, &offset))
            return 0;
    for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES; stage++)
        for (u32 i = 0; i < NVN_DRAW_UNIFORM_SLOTS; i++)
            if ((command->uniform_bound[stage] & (1u << i)) &&
                !vk_buf_get_strict(command->uniform[stage][i].address, command->uniform[stage][i].size, &buffer,
                                   &offset))
                return 0;
    if (index_bytes && !vk_buf_get_strict(command->index_address, index_bytes, &buffer, &offset)) return 0;
    int has_target = command->color_target_mask || command->depth_target_bound;
    if (!has_target || !command->target_complete) return NVN_DRAW_PREPARED;
    for (u32 i = 0; i < 8; i++) {
        if (!(command->color_target_mask & (1u << i))) continue;
        const NvnTextureTarget* target = &command->color_target[i];
        const Surf* surf = vk_rt_find(target->gpu_address);
        VkFmt format = nvn_format(target->format);
        if (!surf) return NVN_DRAW_PREPARED | NVN_DRAW_TARGET_NO_SURFACE;
        if (!format.vk || surf->f.vk != format.vk) {
            LOG_LIMIT(8, "nvn-draw-target-format: color nvn=%x vk=%u surf-vk=%u surf-key=%x\n", target->format,
                      format.vk, surf->f.vk, surf->fkey);
            return NVN_DRAW_PREPARED | NVN_DRAW_TARGET_FORMAT;
        }
        if (surf->w != target->width || surf->h != target->height) {
            LOG_LIMIT(8, "nvn-draw-target-size: color nvn=%ux%u surf=%ux%u\n", target->width, target->height, surf->w,
                      surf->h);
            return NVN_DRAW_PREPARED | NVN_DRAW_TARGET_SIZE;
        }
        if (target->view.base_level || target->view.first_layer) {
            LOG_LIMIT(8, "nvn-draw-target-view: color level=%u layer=%u\n", target->view.base_level,
                      target->view.first_layer);
            return NVN_DRAW_PREPARED | NVN_DRAW_TARGET_VIEW;
        }
    }
    if (command->depth_target_bound) {
        const NvnTextureTarget* target = &command->depth_target;
        const Surf* surf = vk_rt_find(target->gpu_address);
        VkFmt format = nvn_format(target->format);
        if (!surf) return NVN_DRAW_PREPARED | NVN_DRAW_TARGET_NO_SURFACE;
        if (!format.vk || surf->f.vk != format.vk) {
            LOG_LIMIT(8, "nvn-draw-target-format: depth nvn=%x vk=%u surf-vk=%u surf-key=%x\n", target->format,
                      format.vk, surf->f.vk, surf->fkey);
            return NVN_DRAW_PREPARED | NVN_DRAW_TARGET_FORMAT;
        }
        if (surf->w != target->width || surf->h != target->height) {
            LOG_LIMIT(8, "nvn-draw-target-size: depth nvn=%ux%u surf=%ux%u\n", target->width, target->height, surf->w,
                      surf->h);
            return NVN_DRAW_PREPARED | NVN_DRAW_TARGET_SIZE;
        }
        if (target->view.base_level || target->view.first_layer) {
            LOG_LIMIT(8, "nvn-draw-target-view: depth level=%u layer=%u\n", target->view.base_level,
                      target->view.first_layer);
            return NVN_DRAW_PREPARED | NVN_DRAW_TARGET_VIEW;
        }
    }
    return NVN_DRAW_PREPARED | NVN_DRAW_SHARED_TARGET;
}
