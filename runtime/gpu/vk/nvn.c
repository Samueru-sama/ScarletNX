/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN-owned probe images. They remain separate from guest Maxwell surfaces until whole-list replacement and
   guest-memory coherence are implemented. */
#include "gpu/vk/vk.h"
#include "gpu/gpu.h"
#include "gpu/nvn_pool.h"
#include "gpu/shader/shader.h"
#include "hle/nvdrv.h"
#include "platform/log.h"
#include <stdlib.h>
#include <string.h>

#define NVN_IMAGE_SLOTS  128
#define NVN_IMAGE_BUDGET (256ull << 20)
typedef struct {
    VkImageView view;
    VkFormat format;
    u16 mip, layer;
} NvnAttachmentView;
typedef struct {
    u64 texture, pool, address, texture_generation, pool_generation;
    u64 subresource_sequence[16][64];
    u32 width, height, levels, layers, target, format;
    u64 bytes;
    u64 last_batch;
    VkImage image;
    VkImageView sampled_view, base_view, sampled_view_alt, base_view_alt;
    VkFormat alt_format;
    NvnAttachmentView attachments[16];
    NvnAttachmentView storage_views[16];
    VmaAllocation allocation;
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    u64 color_layers[16], depth_layers[16];
    int sampled;
    int attachment;
    int storage;
} NvnImage;
static NvnImage images[NVN_IMAGE_SLOTS];
static u64 image_bytes;
static u64 batch_serial;

void vk_nvn_batch_begin(void) {
    batch_serial++;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        memset(images[i].color_layers, 0, sizeof images[i].color_layers);
        memset(images[i].depth_layers, 0, sizeof images[i].depth_layers);
    }
}

static NvnImage* evict_old_image(void) {
    NvnImage* oldest = NULL;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        NvnImage* image = &images[i];
        if (!image->image || image->last_batch == batch_serial) continue;
        if (!oldest || image->last_batch < oldest->last_batch) oldest = image;
    }
    if (!oldest) return NULL;
    for (u32 i = 0; i < 16; i++)
        if (oldest->attachments[i].view)
            vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, oldest->attachments[i].view, VK_NULL_HANDLE);
    for (u32 i = 0; i < 16; i++)
        if (oldest->storage_views[i].view)
            vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, oldest->storage_views[i].view, VK_NULL_HANDLE);
    if (oldest->sampled_view_alt || oldest->base_view_alt)
        vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, oldest->sampled_view_alt, oldest->base_view_alt);
    vk_defer_image(oldest->image, oldest->allocation, oldest->sampled_view, oldest->base_view);
    image_bytes -= oldest->bytes;
    memset(oldest, 0, sizeof *oldest);
    LOG_LIMIT(1, "nvn-image-cache: evicting images from earlier batches when capacity is needed\n");
    return oldest;
}

static VkFmt nvn_format(u32 format) {
    switch (format) {
        case 0x01: return (VkFmt){.vk = VK_FORMAT_R8_UNORM, .bpp = 1};            /* R8 */
        case 0x05: return (VkFmt){.vk = VK_FORMAT_R16_SFLOAT, .bpp = 2};          /* R16F */
        case 0x0a: return (VkFmt){.vk = VK_FORMAT_R32_SFLOAT, .bpp = 4};          /* R32F */
        case 0x0b: return (VkFmt){.vk = VK_FORMAT_R32_UINT, .bpp = 4, .is_int = 1}; /* R32UI */
        case 0x0c: return (VkFmt){.vk = VK_FORMAT_R32_SINT, .bpp = 4, .is_int = 1, .sint = 1}; /* R32I */
        case 0x0d: return (VkFmt){.vk = VK_FORMAT_R8G8_UNORM, .bpp = 2};          /* RG8 */
        case 0x11: return (VkFmt){.vk = VK_FORMAT_R16G16_SFLOAT, .bpp = 4};       /* RG16F */
        case 0x12: return (VkFmt){.vk = VK_FORMAT_R16G16_UNORM, .bpp = 4};        /* RG16 */
        case 0x13: return (VkFmt){.vk = VK_FORMAT_R16G16_SNORM, .bpp = 4};        /* RG16SN */
        case 0x25: return vk_rt_format(0xd5);                                     /* RGBA8 */
        case 0x29: return (VkFmt){.vk = VK_FORMAT_R16G16B16A16_SFLOAT, .bpp = 8}; /* RGBA16F */
        case 0x2e: return vk_rt_format(0xc0);                                     /* RGBA32F */
        case 0x38: return (VkFmt){.vk = VK_FORMAT_R8G8B8A8_SRGB, .bpp = 4};       /* RGBA8 SRGB */
        case 0x3d: return vk_rt_format(0xd1);                                     /* RGB10A2 */
        case 0x3f: return vk_rt_format(0xe0);                                     /* R11G11B10F */
        case 0x32: return vk_zt_format(0x13);                                     /* D16 */
        case 0x34: return vk_zt_format(0x0a);                                     /* D32F */
        case 0x35: return vk_zt_format(0x14);                                     /* D24S8 */
        case 0x41: return (VkFmt){.vk = VK_FORMAT_BC1_RGB_UNORM_BLOCK, .bpp = 8, .bw = 4, .bh = 4};
        case 0x42: return (VkFmt){.vk = VK_FORMAT_BC1_RGBA_UNORM_BLOCK, .bpp = 8, .bw = 4, .bh = 4};
        case 0x43: return (VkFmt){.vk = VK_FORMAT_BC2_UNORM_BLOCK, .bpp = 16, .bw = 4, .bh = 4};
        case 0x44: return (VkFmt){.vk = VK_FORMAT_BC3_UNORM_BLOCK, .bpp = 16, .bw = 4, .bh = 4};
        case 0x45: return (VkFmt){.vk = VK_FORMAT_BC1_RGB_SRGB_BLOCK, .bpp = 8, .bw = 4, .bh = 4};
        case 0x46: return (VkFmt){.vk = VK_FORMAT_BC1_RGBA_SRGB_BLOCK, .bpp = 8, .bw = 4, .bh = 4};
        case 0x47: return (VkFmt){.vk = VK_FORMAT_BC2_SRGB_BLOCK, .bpp = 16, .bw = 4, .bh = 4};
        case 0x48: return (VkFmt){.vk = VK_FORMAT_BC3_SRGB_BLOCK, .bpp = 16, .bw = 4, .bh = 4};
        default: return (VkFmt){0};
    }
}

static NvnImage* image_for(const NvnTextureTarget* target, VkFormat format, VkImageAspectFlags aspect, int* full) {
    /* Only the RGBA8 UNORM/sRGB pair has a verified compatible view path. Other reinterpretations need their own
       format-class and feature checks before sharing one backing image. */
    int rgba8_pair = (target->storage_format == 0x25 && target->format == 0x38) ||
                     (target->storage_format == 0x38 && target->format == 0x25);
    if (target->format != target->storage_format && !rgba8_pair) {
        LOG_LIMIT(4, "nvn-image-view-format: backing=%x view=%x\n", target->storage_format, target->format);
        return NULL;
    }
    VkFmt backing_format = nvn_format(target->storage_format);
    if (!backing_format.vk) return NULL;
    if (rgba8_pair &&
        (backing_format.vk != (target->storage_format == 0x25 ? VK_FORMAT_R8G8B8A8_UNORM
                                                            : VK_FORMAT_R8G8B8A8_SRGB) ||
         format != (target->format == 0x25 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R8G8B8A8_SRGB)))
        return NULL;
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
            image->height == target->height && image->levels == target->levels && image->layers == target->depth &&
            image->target == target->storage_target && image->format == target->storage_format)
            return image;
    }
    VkFmt guest_format = backing_format;
    const VkPhysicalDeviceLimits* limits = &g_vk.props.limits;
    int is_3d = target->storage_target == 2;
    if (!guest_format.bpp || !target->levels || !target->depth ||
        target->depth > (is_3d ? limits->maxImageDimension3D : limits->maxImageArrayLayers) ||
        target->width > (is_3d ? limits->maxImageDimension3D : limits->maxImageDimension2D) ||
        target->height > (is_3d ? limits->maxImageDimension3D : limits->maxImageDimension2D))
        return NULL;
    if (target->storage_target == 8 &&
        (target->width != target->height || target->width > limits->maxImageDimensionCube || target->depth != 6))
        return NULL;
    u32 max_dimension = target->width > target->height ? target->width : target->height;
    if (is_3d && target->depth > max_dimension) max_dimension = target->depth;
    u32 max_levels = 1;
    while (max_dimension >> max_levels) max_levels++;
    if (target->levels > max_levels || target->levels > 16) return NULL;
    u32 block_width = guest_format.bw ? guest_format.bw : 1;
    u32 block_height = guest_format.bh ? guest_format.bh : 1;
    u64 bytes = 0;
    for (u32 level = 0; level < target->levels; level++) {
        u32 width = target->width >> level ? target->width >> level : 1;
        u32 height = target->height >> level ? target->height >> level : 1;
        bytes += (u64)((width + block_width - 1) / block_width) * ((height + block_height - 1) / block_height) *
                 guest_format.bpp * (is_3d ? (target->depth >> level ? target->depth >> level : 1) : target->depth);
    }
    if (bytes > NVN_IMAGE_BUDGET) return NULL;
    while (bytes > NVN_IMAGE_BUDGET - image_bytes)
        if (!evict_old_image()) {
            *full = 1;
            return NULL;
        }
    if (!empty)
        for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++)
            if (!images[i].image) {
                empty = &images[i];
                break;
            }
    if (!empty) empty = evict_old_image();
    if (!empty) {
        *full = 1;
        return NULL;
    }
    VkFormatProperties properties, view_properties;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, backing_format.vk, &properties);
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, format, &view_properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) return NULL;
    int sampled = !!((properties.optimalTilingFeatures & view_properties.optimalTilingFeatures) &
                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    VkFormatFeatureFlags attachment_feature = aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                                                  ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                  : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    int attachment = !!((properties.optimalTilingFeatures & view_properties.optimalTilingFeatures) &
                       attachment_feature);
    int storage = !!((properties.optimalTilingFeatures & view_properties.optimalTilingFeatures) &
                    VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && !rgba8_pair;
    VkImageUsageFlags attachment_usage =
        attachment ? (aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                         : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                   : 0;
    int mutable_rgba8 = backing_format.vk == VK_FORMAT_R8G8B8A8_UNORM ||
                        backing_format.vk == VK_FORMAT_R8G8B8A8_SRGB;
    VkFormat pair_formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo format_list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                               .viewFormatCount = 2,
                                               .pViewFormats = pair_formats};
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .pNext = mutable_rgba8 ? &format_list : NULL,
                              .imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                              .flags = (is_3d ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0) |
                                       (target->storage_target == 8 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                                       (mutable_rgba8 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0),
                              .format = backing_format.vk,
                              .extent = {target->width, target->height, is_3d ? target->depth : 1},
                              .mipLevels = target->levels,
                              .arrayLayers = is_3d ? 1 : target->depth,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
                                       attachment_usage | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0)};
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
    empty->levels = target->levels;
    empty->layers = target->depth;
    empty->target = target->storage_target;
    empty->format = target->storage_format;
    empty->bytes = bytes;
    image_bytes += bytes;
    empty->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    empty->stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    empty->sampled = sampled;
    empty->attachment = attachment;
    empty->storage = storage;
    if (sampled) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = image,
                                      .viewType = is_3d                         ? VK_IMAGE_VIEW_TYPE_3D
                                                  : target->storage_target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE
                                                  : target->storage_target == 4 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                                : VK_IMAGE_VIEW_TYPE_2D,
                                      .format = backing_format.vk,
                                      .subresourceRange = {aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                                                               ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                               : VK_IMAGE_ASPECT_COLOR_BIT,
                                                           0, target->levels, 0, is_3d ? 1u : target->depth}};
        if (vkCreateImageView(g_vk.dev, &view, NULL, &empty->sampled_view) != VK_SUCCESS) empty->sampled = 0;
        if (empty->sampled && target->levels > 1) {
            view.subresourceRange.levelCount = 1;
            vkCreateImageView(g_vk.dev, &view, NULL, &empty->base_view);
        }
        if (empty->sampled && mutable_rgba8) {
            empty->alt_format = backing_format.vk == VK_FORMAT_R8G8B8A8_UNORM
                                    ? VK_FORMAT_R8G8B8A8_SRGB
                                    : VK_FORMAT_R8G8B8A8_UNORM;
            view.format = empty->alt_format;
            view.subresourceRange.levelCount = target->levels;
            if (vkCreateImageView(g_vk.dev, &view, NULL, &empty->sampled_view_alt) != VK_SUCCESS)
                empty->alt_format = VK_FORMAT_UNDEFINED;
            if (empty->sampled_view_alt && target->levels > 1) {
                view.subresourceRange.levelCount = 1;
                vkCreateImageView(g_vk.dev, &view, NULL, &empty->base_view_alt);
            }
        }
    }
    return empty;
}

/* Materialize the exact mip and layer a future host draw would attach. No pixels are marked written until that draw
   actually executes on the host path. */
static int prepare_attachment(const NvnTextureTarget* target, int depth) {
    u32 mip = target->view.base_level, layer = target->view.first_layer;
    if ((target->storage_target != 1 && target->storage_target != 2 && target->storage_target != 4 &&
         target->storage_target != 8) ||
        (target->target != target->storage_target && target->target != 1) || !target->width || !target->height ||
        !target->depth || (target->storage_target == 1 && target->depth != 1) || !target->levels ||
        target->samples > 1 || mip >= target->levels ||
        layer >= (target->storage_target == 2 ? (target->depth >> mip ? target->depth >> mip : 1) : target->depth) ||
        target->view.level_count > 1 ||
        target->view.layer_count > 1 || (target->view.flags & (8 | 16))) {
        LOG_LIMIT(8, "nvn-attachment-shape: target=%u storage=%u size=%ux%ux%u levels=%u samples=%u view=%x/%u/%u/%u/%u\n",
                  target->target, target->storage_target, target->width, target->height, target->depth,
                  target->levels, target->samples, target->view.flags, mip, layer, target->view.level_count,
                  target->view.layer_count);
        return 0;
    }
    VkFmt format = nvn_format(target->format);
    if (!format.vk || !!format.depth != depth || format.bw > 1 || format.bh > 1 || format.conv ||
        (target->format == 0x25 && format.vk != VK_FORMAT_R8G8B8A8_UNORM) ||
        (target->format == 0x3f && format.vk != VK_FORMAT_B10G11R11_UFLOAT_PACK32)) {
        LOG_LIMIT(8, "nvn-attachment-format: format=%x vk=%u depth=%u storage=%x\n", target->format,
                  format.vk, depth, target->storage_format);
        return 0;
    }
    int full = 0;
    NvnImage* image = image_for(target, format.vk, vk_aspect(&format), &full);
    if (!image || !image->attachment) {
        LOG_LIMIT(8, "nvn-attachment-image: format=%x storage=%x full=%d image=%d attachment=%d\n",
                  target->format, target->storage_format, full, !!image, image && !!image->attachment);
        return 0;
    }
    for (u32 i = 0; i < 16; i++)
        if (image->attachments[i].view && image->attachments[i].format == format.vk &&
            image->attachments[i].mip == mip && image->attachments[i].layer == layer) {
            image->last_batch = batch_serial;
            return 1;
        }
    for (u32 i = 0; i < 16; i++) {
        NvnAttachmentView* slot = &image->attachments[i];
        if (slot->view) continue;
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = image->image,
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = format.vk,
                                      .subresourceRange = {vk_aspect(&format), mip, 1, layer, 1}};
        if (vkCreateImageView(g_vk.dev, &view, NULL, &slot->view) != VK_SUCCESS) return 0;
        slot->mip = mip;
        slot->layer = layer;
        slot->format = format.vk;
        image->last_batch = batch_serial;
        return 1;
    }
    return 0;
}

/* Allocate the exact single-mip, single-layer storage view a future NVN draw will bind. This preflight neither
   writes pixels nor changes image layout; the draw path must order storage writes with later texture reads. */
static int prepare_storage_image(const NvnTextureTarget* target, VkFormat expected) {
    u32 mip = target->view.base_level, layer = target->view.first_layer;
    if (target->storage_target != 1 || target->target != 1 || !target->width || !target->height ||
        target->depth != 1 || !target->levels || target->samples > 1 || mip >= target->levels ||
        target->view.level_count > 1 || target->view.layer_count > 1 || layer ||
        (target->view.flags & (8 | 16)))
        return 0;
    VkFmt format = nvn_format(target->format);
    if (format.vk != expected || target->format != target->storage_format || !format.is_int) return 0;
    int full = 0;
    NvnImage* image = image_for(target, format.vk, VK_IMAGE_ASPECT_COLOR_BIT, &full);
    if (!image || !image->storage) return 0;
    for (u32 i = 0; i < 16; i++)
        if (image->storage_views[i].view && image->storage_views[i].format == format.vk &&
            image->storage_views[i].mip == mip && image->storage_views[i].layer == layer) {
            image->last_batch = batch_serial;
            return 1;
        }
    for (u32 i = 0; i < 16; i++) {
        NvnAttachmentView* slot = &image->storage_views[i];
        if (slot->view) continue;
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = image->image,
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = format.vk,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, layer, 1}};
        if (vkCreateImageView(g_vk.dev, &view, NULL, &slot->view) != VK_SUCCESS) return 0;
        slot->mip = mip;
        slot->layer = layer;
        slot->format = format.vk;
        image->last_batch = batch_serial;
        return 1;
    }
    return 0;
}

int vk_nvn_clear(const NvnClearCommand* command) {
    const NvnTextureTarget* target = &command->target;
    /* vkCmdClearColorImage interprets the image's backing format, not an alternate sRGB attachment view. */
    if (target->format != target->storage_format) return 0;
    u32 mip = target->view.base_level;
    u32 layer = target->view.first_layer;
    if ((target->storage_target != 1 && target->storage_target != 4 && target->storage_target != 8) ||
        (target->target != target->storage_target && target->target != 1) || !target->width || !target->height ||
        !target->depth || (target->storage_target == 1 && target->depth != 1) || !target->levels ||
        target->samples > 1 || mip >= target->levels || mip >= 16 || layer >= target->depth ||
        target->view.level_count > 1 || target->view.layer_count > 1 || (target->view.flags & (8 | 16))) {
        LOG_LIMIT(12, "nvn-clear-shape: target=%u backing=%u %ux%ux%u levels=%u samples=%u view=%x/%u/%u/%u/%u\n",
                  target->target, target->storage_target, target->width, target->height, target->depth, target->levels,
                  target->samples, target->view.flags, mip, layer, target->view.level_count, target->view.layer_count);
        return 0;
    }
    VkFmt format = nvn_format(target->format);
    if (!format.vk) {
        LOG_LIMIT(12, "nvn-clear-format: format=%x unsupported\n", target->format);
        return 0;
    }
    if ((target->format == 0x25 && format.vk != VK_FORMAT_R8G8B8A8_UNORM) ||
        (target->format == 0x3f && format.vk != VK_FORMAT_B10G11R11_UFLOAT_PACK32))
        return 0; /* vk_rt_format may substitute RGBA8 on unsupported devices */
    if (command->kind == NVN_CLEAR_COLOR) {
        if (format.depth || format.bw > 1 || format.bh > 1 || command->mask != 15) {
            LOG_LIMIT(12, "nvn-clear-color-mask: format=%x mask=%x\n", target->format, command->mask);
            return 0;
        }
    } else if (command->kind == NVN_CLEAR_DEPTH_STENCIL) {
        if (!format.depth ||
            (command->stencil_mask && command->stencil_mask != 0xff && command->stencil_mask != 0xffffffffu) ||
            (!command->depth_mask && !command->stencil_mask) || (command->stencil_mask && !format.stencil))
            return 0;
    } else {
        return 0;
    }
    VkImageAspectFlags aspect = vk_aspect(&format);
    int full = 0;
    NvnImage* image = image_for(target, format.vk, aspect, &full);
    if (!image) {
        LOG_LIMIT(12, "nvn-clear-image-reject: format=%x %ux%ux%u levels=%u cache-full=%d\n", target->format,
                  target->width, target->height, target->depth, target->levels, full);
        return full ? -1 : 0;
    }
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_barrier(cb, image->image, aspect, image->stage, image->access, image->layout, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image->stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    image->access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkImageSubresourceRange range = {aspect, mip, 1, layer, 1};
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
    if (layer < 64 && (range.aspectMask & (VK_IMAGE_ASPECT_COLOR_BIT | VK_IMAGE_ASPECT_DEPTH_BIT))) {
        image->subresource_sequence[mip][layer] = command->sequence;
        if (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) image->color_layers[mip] |= 1ull << layer;
        if (range.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) image->depth_layers[mip] |= 1ull << layer;
    }
    image->last_batch = batch_serial;
    return 1;
}

int vk_nvn_copy_buffer_texture(const NvnCopyBufferTextureCommand* command) {
    const NvnTextureTarget* target = &command->target;
    u32 mip = target->view.base_level;
    u32 mip_width = mip < 16 ? target->width >> mip : 0;
    u32 mip_height = mip < 16 ? target->height >> mip : 0;
    if (!mip_width) mip_width = 1;
    if (!mip_height) mip_height = 1;
    if (target->target != 1 || !target->width || !target->height || target->depth != 1 || !target->levels ||
        target->samples > 1 || mip >= target->levels || mip >= 16 || target->view.first_layer ||
        target->view.level_count > 1 || target->view.layer_count > 1 || (target->view.flags & (8 | 16)) ||
        command->flags || command->x || command->y || command->z || command->width != (i32)mip_width ||
        command->height != (i32)mip_height || command->depth != 1 || !command->source) {
        LOG_LIMIT(
            2,
            "nvn-copy-shape: target=%u %ux%ux%u levels=%u samples=%u view=%x/%u/%u/%u/%u region=%d,%d,%d %dx%dx%d flags=%x source=%llx\n",
            target->target, target->width, target->height, target->depth, target->levels, target->samples,
            target->view.flags, target->view.base_level, target->view.first_layer, target->view.level_count,
            target->view.layer_count, command->x, command->y, command->z, command->width, command->height,
            command->depth, command->flags, (unsigned long long)command->source);
        return 0;
    }
    VkFmt format = nvn_format(target->format);
    if (!format.vk || format.depth || format.conv ||
        (target->format == 0x25 && format.vk != VK_FORMAT_R8G8B8A8_UNORM) ||
        (target->format == 0x3f && format.vk != VK_FORMAT_B10G11R11_UFLOAT_PACK32)) {
        LOG_LIMIT(2, "nvn-copy-format: format=%x vk=%u depth=%u conv=%u block=%ux%u\n", target->format, format.vk,
                  format.depth, format.conv, format.bw, format.bh);
        return 0;
    }
    u32 block_width = format.bw ? format.bw : 1;
    u32 block_height = format.bh ? format.bh : 1;
    u64 row = (u64)((mip_width + block_width - 1) / block_width) * format.bpp;
    u64 size = row * ((mip_height + block_height - 1) / block_height);
    if (!size || size > (64ull << 20) || (command->row_stride && command->row_stride != (i64)row) ||
        (command->image_stride && command->image_stride != (i64)size)) {
        LOG_LIMIT(2, "nvn-copy-stride: format=%x size=%llu row=%llu configured=%lld image=%lld\n", target->format,
                  (unsigned long long)size, (unsigned long long)row, (long long)command->row_stride,
                  (long long)command->image_stride);
        return 0;
    }
    if (command->source > UINT64_MAX - size) return 0;
    for (u64 address = command->source; address < command->source + size;) {
        if (!gmmu_translate(address)) {
            LOG_LIMIT(2, "nvn-copy-source: unmapped address=%llx size=%llu\n", (unsigned long long)address,
                      (unsigned long long)size);
            return 0;
        }
        u64 next = (address & ~0xfffull) + 0x1000;
        address = next < command->source + size ? next : command->source + size;
    }
    u8* source = malloc(size);
    if (!source) return 0;
    gmem_read(command->source, source, size);
    int full = 0;
    NvnImage* image = image_for(target, format.vk, VK_IMAGE_ASPECT_COLOR_BIT, &full);
    if (!image) {
        LOG_LIMIT(2, "nvn-copy-image: format=%x size=%ux%u levels=%u cache-full=%d\n", target->format, target->width,
                  target->height, target->levels, full);
        free(source);
        return full ? -1 : 0;
    }
    VkBuffer buffer;
    u64 offset;
    u8* staged = vk_stage(size, 16, &buffer, &offset);
    if (!staged) {
        LOG_LIMIT(2, "nvn-copy-stage: bytes=%llu\n", (unsigned long long)size);
        free(source);
        return 0;
    }
    memcpy(staged, source, size);
    free(source);
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_barrier(cb, image->image, VK_IMAGE_ASPECT_COLOR_BIT, image->stage, image->access, image->layout,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = {.bufferOffset = offset,
                                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1},
                                .imageExtent = {mip_width, mip_height, 1}};
    vkCmdCopyBufferToImage(cb, buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    image->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image->stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    image->access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    image->subresource_sequence[mip][0] = command->sequence;
    image->color_layers[mip] |= 1;
    image->last_batch = batch_serial;
    return 1;
}

static VkImageView image_view_from_semantic(const NvnDescriptorSnapshot* descriptor, u64 last_unmodeled_write) {
    u64 address;
    u32 width, height, layers, levels;
    VkImageViewType view_type;
    VkFormat format;
    if (!descriptor->texture || !descriptor->texture_generation ||
        !vk_tex_tic_shadow(descriptor->tic, &address, &width, &height, &layers, &levels, &view_type, &format))
        return VK_NULL_HANDLE;
    if (layers > 64) return VK_NULL_HANDLE;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        NvnImage* image = &images[i];
        VkImageViewType image_type = image->target == 8   ? VK_IMAGE_VIEW_TYPE_CUBE
                                     : image->target == 4 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                          : VK_IMAGE_VIEW_TYPE_2D;
        if (!image->image || !image->sampled || image_type != view_type || image->layers != layers ||
            image->texture != descriptor->texture || image->texture_generation != descriptor->texture_generation ||
            image->pool != descriptor->memory_pool || image->pool_generation != descriptor->memory_pool_generation ||
            image->address != address || image->width != width || image->height != height)
            continue;
        VkFmt source = nvn_format(image->format);
        int alt = source.vk != format;
        if (alt && image->alt_format != format) continue;
        VkImageView sampled_view = levels == 1 && image->levels > 1
                                       ? (alt ? image->base_view_alt : image->base_view)
                                       : (alt ? image->sampled_view_alt : image->sampled_view);
        if (!sampled_view || (levels != image->levels && levels != 1)) continue;
        int complete = 1;
        for (u32 level = 0; level < levels; level++)
            for (u32 layer = 0; layer < layers; layer++) {
                u64 written = source.depth ? image->depth_layers[level] : image->color_layers[level];
                if (!(written & (1ull << layer)) || image->subresource_sequence[level][layer] <= last_unmodeled_write)
                    complete = 0;
            }
        if (!complete) continue;
        VkCommandBuffer cb = vk_cmd();
        vk_barrier(cb, image->image, vk_aspect(&source), image->stage, image->access, image->layout,
                   VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image->stage = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        image->access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        return sampled_view;
    }
    return VK_NULL_HANDLE;
}

/* Only the NVN blob wrapper and Maxwell SPH are needed to select a stage. An unrecognized
   reference must stay unresolved; SetShadersExt does not always put code in its first word. */
static int shader_code_stage(u64 address, u32* type) {
    if (!address || address > UINT64_MAX - 0x34) return -1;
    u32 header[2], sph;
    gmem_read_code(address, header, sizeof header);
    if (header[0] != 0x12345678 || (header[1] && (header[1] < 0x80 || header[1] > 0x10030))) return -1;
    gmem_read_code(address + 0x30, &sph, sizeof sph);
    *type = (sph >> 10) & 15;
    if ((sph & 31) == 1 && *type <= 1) return ST_VS;
    if ((sph & 31) == 2 && *type == 5) return ST_FS;
    return -1;
}

/* The NVN attribute state stores NVNformat at bits 21-30. Only true integer formats
   change the shader input type; the UI2F/I2F variants still feed floating-point inputs. */
static int nvn_integer_format(u32 format) {
    switch (format) {
        case 0x04:
        case 0x09:
        case 0x0c:
        case 0x10:
        case 0x15:
        case 0x18:
        case 0x1c:
        case 0x21:
        case 0x24:
        case 0x28:
        case 0x2d:
        case 0x30:
        case 0x6a:
        case 0x70:
        case 0x75:
        case 0x78: return -1;
        case 0x03:
        case 0x08:
        case 0x0b:
        case 0x0f:
        case 0x14:
        case 0x17:
        case 0x1b:
        case 0x20:
        case 0x23:
        case 0x27:
        case 0x2c:
        case 0x2f:
        case 0x3e:
        case 0x6f:
        case 0x74:
        case 0x77: return 1;
        default: return 0;
    }
}

static VkFormat nvn_vertex_format(u32 format) {
    switch (format) {
        case 0x03: return VK_FORMAT_R8_UINT;
        case 0x0d: return VK_FORMAT_R8G8_UNORM;
        case 0x0f: return VK_FORMAT_R8G8_UINT;
        case 0x11: return VK_FORMAT_R16G16_SFLOAT;
        case 0x12: return VK_FORMAT_R16G16_UNORM;
        case 0x16: return VK_FORMAT_R32G32_SFLOAT;
        case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
        case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
        case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
        case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
        case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 0x2a: return VK_FORMAT_R16G16B16A16_UNORM;
        case 0x2e: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
}

static int prepare_vertex_formats(const NvnDrawCommand* command, u32 shader_inputs) {
    for (u32 i = 0; i < command->vertex_attrib_count; i++) {
        if (!(shader_inputs & (1u << i))) continue;
        const NvnVertexAttrib* attrib = &command->vertex_attrib[i];
        if (!attrib->enabled) continue;
        VkFormat format = nvn_vertex_format(attrib->format);
        if (!format) {
            LOG_LIMIT(8, "nvn-vertex-format: unmapped format=%x attribute=%u\n", attrib->format, i);
            return 0;
        }
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, format, &properties);
        if (!(properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
            LOG_LIMIT(8, "nvn-vertex-format: device rejected format=%x vk=%u attribute=%u\n", attrib->format, format,
                      i);
            return 0;
        }
    }
    return 1;
}

static int materialize_draw_shaders(const NvnDrawCommand* command, VkSh** vertex, VkSh** fragment_shader) {
    if ((command->program_bound & 3) != 3 || !command->shader_refs_known) return 0;
    u64 vertex_a = 0, vertex_b = 0, fragment = 0;
    /* BindProgram may leave older bindings in the other four stage slots. Select the
       vertex and fragment programs explicitly, even when both slots name one program. */
    for (u32 stage = 0; stage < 2; stage++) {
        const NvnProgramSnapshot* program = &command->program[stage].snapshot;
        if (!command->program[stage].handle) return 0;
        if (!program->refs_known || !program->shader_count || program->shader_count > NVN_PROGRAM_MAX_SHADERS) return 0;
        for (u32 i = 0; i < program->shader_count; i++) {
            u32 type = 0;
            u64 address = program->refs[i].code_candidate;
            int found = shader_code_stage(address, &type);
            if (stage == 0 && found == ST_VS) {
                u64* slot = type == 0 ? &vertex_a : &vertex_b;
                if (*slot && *slot != address + 0x30) {
                    LOG_LIMIT(8, "nvn-shader-module: duplicate vertex type=%u old=%llx new=%llx\n", type,
                              (unsigned long long)*slot, (unsigned long long)(address + 0x30));
                    return 0;
                }
                *slot = address + 0x30;
            } else if (stage == 1 && found == ST_FS) {
                if (fragment && fragment != address + 0x30) {
                    LOG_LIMIT(8, "nvn-shader-module: duplicate fragment old=%llx new=%llx\n",
                              (unsigned long long)fragment, (unsigned long long)(address + 0x30));
                    return 0;
                }
                fragment = address + 0x30;
            } else if (found < 0) {
                LOG_LIMIT(8, "nvn-shader-module: unresolved ref stage=%u index=%u code=%llx extension=%llx\n", stage, i,
                          (unsigned long long)address, (unsigned long long)program->refs[i].extension);
                return 0;
            }
        }
    }
    if ((!vertex_a && !vertex_b) || !fragment) {
        LOG_LIMIT(8, "nvn-shader-module: missing stage vertex-a=%llx vertex-b=%llx fragment=%llx bound=%x\n",
                  (unsigned long long)vertex_a, (unsigned long long)vertex_b, (unsigned long long)fragment,
                  command->program_bound);
        return 0;
    }
    u32 attr_int = 0, attr_sint = 0;
    for (u32 i = 0; i < command->vertex_attrib_count; i++) {
        const NvnVertexAttrib* attrib = &command->vertex_attrib[i];
        if (!attrib->enabled) continue;
        int kind = nvn_integer_format(attrib->format);
        if (kind) {
            attr_int |= 1u << i;
            if (kind < 0) attr_sint |= 1u << i;
        }
    }
    u32 rt_int = 0, rt_sint = 0;
    for (u32 i = 0; i < 8; i++) {
        if (!(command->color_target_mask & (1u << i))) continue;
        int kind = nvn_integer_format(command->color_target[i].format);
        if (kind) {
            rt_int |= 1u << i;
            if (kind < 0) rt_sint |= 1u << i;
        }
    }
    for (u32 stage = 0; stage < 2; stage++) {
        u32 row = stage ? 4 : 0;
        u64 saved_cb[18];
        u32 saved_size[18];
        memcpy(saved_cb, g_vk_cb[row], sizeof saved_cb);
        memcpy(saved_size, g_vk_cbsize[row], sizeof saved_size);
        memset(g_vk_cb[row], 0, sizeof saved_cb);
        memset(g_vk_cbsize[row], 0, sizeof saved_size);
        for (u32 i = 0; i < NVN_DRAW_UNIFORM_SLOTS; i++)
            if (command->uniform_bound[stage] & (1u << i)) {
                g_vk_cb[row][i] = command->uniform[stage][i].address;
                g_vk_cbsize[row][i] = (u32)command->uniform[stage][i].size;
            }
        VkSh* shader = vk_shader_get(stage ? fragment : (vertex_b ? vertex_b : vertex_a),
                                     stage || !vertex_b ? 0 : vertex_a, stage ? ST_FS : ST_VS, stage ? 0 : attr_int,
                                     stage ? 0 : attr_sint, stage ? rt_int : 0, stage ? rt_sint : 0, 0, 0, 0);
        if (stage && shader && shader->mod && !shader->failed && !shader->si.unsupported && shader->si.nimg) {
            u32 image_int = 0, image_sint = 0;
            int images_known = 1;
            for (int k = 0; k < shader->si.nimg; k++) {
                u32 word = shader->si.img[k].cbuf_off;
                if (!shader->si.img[k].bindless || shader->si.img[k].bl_cbuf != 2 || word < 72 ||
                    ((word - 72) & 1) || (word - 72) / 2 >= NVN_DRAW_IMAGE_SLOTS) {
                    images_known = 0;
                    break;
                }
                u32 slot = (word - 72) / 2;
                const NvnDescriptorSnapshot* descriptor = &command->image_descriptor[stage][slot];
                if (!(command->image_bound[stage] & (1u << slot)) ||
                    !(command->image_descriptor_valid[stage] & (1u << slot)) || !descriptor->tic_valid) {
                    images_known = 0;
                    break;
                }
                VkFmt format = vk_tic_format(descriptor->tic[0], 0);
                if (!format.vk) {
                    images_known = 0;
                    break;
                }
                if (format.is_int) {
                    image_int |= 1u << k;
                    if (format.sint) image_sint |= 1u << k;
                }
                if (shader->si.img[k].number_type &&
                    (format.vk != VK_FORMAT_R32_UINT || shader->si.img[k].number_type != 1)) {
                    images_known = 0;
                    break;
                }
            }
            if (images_known && (image_int || image_sint))
                shader = vk_shader_get(fragment, 0, ST_FS, 0, 0, rt_int, rt_sint, 0, image_int, image_sint);
            else if (!images_known)
                shader = NULL;
        }
        memcpy(g_vk_cb[row], saved_cb, sizeof saved_cb);
        memcpy(g_vk_cbsize[row], saved_size, sizeof saved_size);
        if (!shader || !shader->mod || shader->failed || shader->si.unsupported) {
            LOG_LIMIT(8, "nvn-shader-module: compile failure stage=%u code=%llx failed=%d unsupported=%d first=%s\n",
                      stage, (unsigned long long)(stage ? fragment : (vertex_b ? vertex_b : vertex_a)),
                      shader ? shader->failed : -1, shader ? shader->si.unsupported : -1,
                      shader ? shader->si.first_unsupported : "unavailable");
            return 0;
        }
        if (stage)
            *fragment_shader = shader;
        else
            *vertex = shader;
    }
    return 1;
}

static VkBlendFactor nvn_blend_factor(u32 factor) {
    switch (factor) {
        case 1: return VK_BLEND_FACTOR_ZERO;
        case 2: return VK_BLEND_FACTOR_ONE;
        case 3: return VK_BLEND_FACTOR_SRC_COLOR;
        case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_DST_ALPHA;
        case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
        case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
        case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_MAX_ENUM;
    }
}

static VkBlendOp nvn_blend_op(u32 equation) {
    switch (equation) {
        case 1: return VK_BLEND_OP_ADD;
        case 2: return VK_BLEND_OP_SUBTRACT;
        case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4: return VK_BLEND_OP_MIN;
        case 5: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_MAX_ENUM;
    }
}

static int nvn_dual_source_factor(u32 factor) {
    return factor == VK_BLEND_FACTOR_SRC1_COLOR || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ||
           factor == VK_BLEND_FACTOR_SRC1_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}

/* This key checks that the observed shader, vertex, blend, polygon, and attachment interface
   can form a Vulkan pipeline. Dynamic raster and depth state is set by a future host draw. */
static int prepare_draw_pipeline(const NvnDrawCommand* command, const VkSh* vertex, const VkSh* fragment_shader) {
    int default_single_sample = !command->multisample_state_known &&
                                (command->color_target_mask || command->depth_target_bound);
    for (u32 i = 0; i < 8 && default_single_sample; i++)
        if ((command->color_target_mask & (1u << i)) && command->color_target[i].samples > 1)
            default_single_sample = 0;
    if (command->depth_target_bound && command->depth_target.samples > 1) default_single_sample = 0;
    if (!vertex || !fragment_shader || !command->channel_mask_known || !command->color_state_known ||
        !command->polygon_state_known || !command->depth_stencil_state_known ||
        (!command->multisample_state_known && !default_single_sample)) {
        LOG_LIMIT(8,
                  "nvn-pipeline: missing base state shader=%d/%d channel=%d color=%d polygon=%d depth=%d samples=%d\n",
                  !!vertex, !!fragment_shader, command->channel_mask_known, command->color_state_known,
                  command->polygon_state_known, command->depth_stencil_state_known, command->multisample_state_known);
        return 0;
    }
    /* The SDK's zero-initialized state is single-sample. Infer it only for captured single-sample attachments. */
    u32 multisample = command->multisample_state_known ? command->multisample_state[0] : 0;
    u32 samples = (multisample >> 8) & 31, raster_samples = (multisample >> 15) & 31;
    /* The current attachment and pipeline path is single-sample only. Zero is the SDK default for one sample. */
    if (samples > 1 || raster_samples > 1 || (multisample & 0x1a) || ((multisample >> 13) & 3) ||
        (multisample & (1u << 20))) {
        LOG_LIMIT(8, "nvn-pipeline: unsupported multisample state=%08x\n", multisample);
        return 0;
    }
    /* The existing pipeline key does not represent logic operations or alpha testing. */
    if (((command->color_state >> 16) & 0xff) != 3 || ((command->color_state >> 24) & 0xf) != 8) {
        LOG_LIMIT(8, "nvn-pipeline: unsupported color state=%08x\n", command->color_state);
        return 0;
    }
    PipeKey key = {0};
    key.vs = vertex->mod;
    key.fs = fragment_shader->mod;
    switch ((command->polygon_state >> 3) & 3) {
        case 0: key.polygon = VK_POLYGON_MODE_POINT; break;
        case 1: key.polygon = VK_POLYGON_MODE_LINE; break;
        case 2: key.polygon = VK_POLYGON_MODE_FILL; break;
        default: LOG_LIMIT(8, "nvn-pipeline: unsupported polygon state=%08x\n", command->polygon_state); return 0;
    }
    if (key.polygon != VK_POLYGON_MODE_FILL && !g_vk.feat.fillModeNonSolid) {
        LOG_LIMIT(8, "nvn-pipeline: polygon mode requires fillModeNonSolid state=%08x\n", command->polygon_state);
        return 0;
    }
    switch (command->primitive) {
        case 0: key.topo_class = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case 1: key.topo_class = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case 3: key.topo_class = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case 4: key.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case 5: key.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case 6: key.topo_class = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
        default: LOG_LIMIT(8, "nvn-pipeline: unsupported primitive=%u\n", command->primitive); return 0;
    }
    const VkPhysicalDeviceLimits* limits = &g_vk.props.limits;
    int constant_binding = -1;
    for (u32 location = 0; location < 32; location++) {
        if (!(vertex->si.in_mask & (1u << location))) continue;
        if (location >= command->vertex_attrib_count || key.nattr >= limits->maxVertexInputAttributes) {
            LOG_LIMIT(8, "nvn-pipeline: missing attribute location=%u count=%u bind=%u\n", location,
                      command->vertex_attrib_count, key.nbind);
            return 0;
        }
        const NvnVertexAttrib* attrib = &command->vertex_attrib[location];
        if (!attrib->enabled || attrib->stream >= command->vertex_stream_count ||
            !(command->vertex_bound & (1u << attrib->stream))) {
            /* Match the Maxwell renderer's unbound-stream fallback: one shared, zero-stride buffer containing
               (0,0,0,1) as float and integer vectors. The future draw path must bind that buffer here. */
            if (constant_binding < 0) {
                if (key.nbind >= limits->maxVertexInputBindings || key.nbind >= 33) return 0;
                constant_binding = (int)key.nbind++;
            }
            int kind = nvn_integer_format(attrib->format);
            key.attr[key.nattr].loc = location;
            key.attr[key.nattr].binding = (u32)constant_binding;
            key.attr[key.nattr].format = kind < 0   ? VK_FORMAT_R32G32B32A32_SINT
                                         : kind > 0 ? VK_FORMAT_R32G32B32A32_UINT
                                                    : VK_FORMAT_R32G32B32A32_SFLOAT;
            key.attr[key.nattr].offset = kind ? 16 : 0;
            key.nattr++;
            continue;
        }
        VkFormat format = nvn_vertex_format(attrib->format);
        if (!format || attrib->offset > limits->maxVertexInputAttributeOffset ||
            command->vertex_stream[attrib->stream].stride > limits->maxVertexInputBindingStride) {
            LOG_LIMIT(8, "nvn-pipeline: invalid format, offset, or stride location=%u format=%x offset=%u stride=%u\n",
                      location, attrib->format, attrib->offset, command->vertex_stream[attrib->stream].stride);
            return 0;
        }
        u32 divisor = command->vertex_stream[attrib->stream].divisor;
        if (divisor > 1 && !g_vk.has_divisor) {
            LOG_LIMIT(8, "nvn-pipeline: unsupported divisor=%u\n", divisor);
            return 0;
        }
        if (key.nbind >= limits->maxVertexInputBindings || key.nbind >= 33) return 0;
        u32 binding = key.nbind++;
        key.bind[binding].divisor = divisor;
        key.attr[key.nattr].loc = location;
        key.attr[key.nattr].binding = binding;
        key.attr[key.nattr].format = format;
        key.attr[key.nattr].offset = attrib->offset;
        key.nattr++;
    }
    for (u32 i = 0; i < 8; i++) {
        if (!(command->color_target_mask & (1u << i))) continue;
        VkFmt format = nvn_format(command->color_target[i].format);
        if (!format.vk || format.depth) {
            LOG_LIMIT(8, "nvn-pipeline: unsupported color format=%x\n", command->color_target[i].format);
            return 0;
        }
        key.color[i] = format.vk;
        key.blend[i].mask = (command->channel_mask >> (i * 4)) & 0xf;
        key.blend[i].en = !!(command->color_state & (1u << (8 + i)));
        if (key.blend[i].en) {
            if (!(command->blend_state_known & (1u << i))) {
                LOG_LIMIT(8, "nvn-pipeline: missing blend state target=%u\n", i);
                return 0;
            }
            const u32* blend = command->blend_state[i];
            if (blend[0] & 0x3f00) {
                LOG_LIMIT(8, "nvn-pipeline: unsupported advanced blend target=%u state=%08x\n", i, blend[0]);
                return 0;
            }
            key.blend[i].sc = nvn_blend_factor(blend[1] & 0x7f);
            key.blend[i].dc = nvn_blend_factor((blend[1] >> 8) & 0x7f);
            key.blend[i].sa = nvn_blend_factor((blend[1] >> 16) & 0x7f);
            key.blend[i].da = nvn_blend_factor((blend[1] >> 24) & 0x7f);
            key.blend[i].cop = nvn_blend_op((blend[0] >> 24) & 0xf);
            key.blend[i].aop = nvn_blend_op((blend[0] >> 28) & 0xf);
            if (key.blend[i].sc == VK_BLEND_FACTOR_MAX_ENUM || key.blend[i].dc == VK_BLEND_FACTOR_MAX_ENUM ||
                key.blend[i].sa == VK_BLEND_FACTOR_MAX_ENUM || key.blend[i].da == VK_BLEND_FACTOR_MAX_ENUM ||
                key.blend[i].cop == VK_BLEND_OP_MAX_ENUM || key.blend[i].aop == VK_BLEND_OP_MAX_ENUM) {
                LOG_LIMIT(8, "nvn-pipeline: unsupported blend target=%u state=%08x:%08x\n", i, blend[0], blend[1]);
                return 0;
            }
        }
        key.ncolor = i + 1;
    }
    if (command->depth_target_bound) {
        VkFmt format = nvn_format(command->depth_target.format);
        if (!format.vk || !format.depth) {
            LOG_LIMIT(8, "nvn-pipeline: unsupported depth format=%x\n", command->depth_target.format);
            return 0;
        }
        key.depth = format.vk;
        if (format.stencil) key.stencil = format.vk;
    }
    u32 depth_state = command->depth_stencil_state[0];
    /* Vulkan skips depth and stencil tests without their respective attachments. The SDK can leave their state
       enabled across a color-only pass, so only validate the active attachment's test configuration. */
    if (key.depth && (depth_state & 1) && (!((depth_state >> 4) & 0xf) || ((depth_state >> 4) & 0xf) > 8)) {
        LOG_LIMIT(8, "nvn-pipeline: unsupported depth compare state=%08x\n", depth_state);
        return 0;
    }
    if (key.stencil && (depth_state & 4)) {
        u32 stencil_state = command->depth_stencil_state[1];
        for (u32 shift = 0; shift < 32; shift += 4) {
            u32 value = (stencil_state >> shift) & 0xf;
            if (!value || value > 8) {
                LOG_LIMIT(8, "nvn-pipeline: unsupported stencil state=%08x\n", stencil_state);
                return 0;
            }
        }
    }
    if (!key.ncolor && !key.depth) return 0;
    for (u32 i = 0; i < key.ncolor; i++) {
        if (key.blend[i].en && !g_vk.feat.dualSrcBlend &&
            (nvn_dual_source_factor(key.blend[i].sc) || nvn_dual_source_factor(key.blend[i].dc) ||
             nvn_dual_source_factor(key.blend[i].sa) || nvn_dual_source_factor(key.blend[i].da))) {
            LOG_LIMIT(8, "nvn-pipeline: dual-source blend unsupported target=%u\n", i);
            return 0;
        }
        if (i && !g_vk.feat.independentBlend && memcmp(&key.blend[i], &key.blend[0], sizeof key.blend[i])) {
            LOG_LIMIT(8, "nvn-pipeline: independent blend unsupported target=%u\n", i);
            return 0;
        }
    }
    return vk_pipeline_get(&key) != VK_NULL_HANDLE;
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
    VkSh* vertex_shader = NULL;
    VkSh* fragment_shader = NULL;
    int shader_ready =
        command->materialize_shaders && materialize_draw_shaders(command, &vertex_shader, &fragment_shader);
    int image_storage_ready = 0;
    if (shader_ready && fragment_shader->si.nimg) {
        image_storage_ready = 1;
        for (int k = 0; k < fragment_shader->si.nimg; k++) {
            u32 word = fragment_shader->si.img[k].cbuf_off;
            if (!fragment_shader->si.img[k].bindless || fragment_shader->si.img[k].bl_cbuf != 2 ||
                word < 72 || ((word - 72) & 1) ||
                (word - 72) / 2 >= NVN_DRAW_IMAGE_SLOTS) {
                image_storage_ready = 0;
                break;
            }
            u32 slot = (word - 72) / 2;
            if (!(command->image_bound[1] & (1u << slot)) ||
                !(command->image_descriptor_valid[1] & (1u << slot)) ||
                !(command->image_target_valid[1] & (1u << slot))) {
                image_storage_ready = 0;
                break;
            }
            VkFormat format = vk_tic_format(command->image_descriptor[1][slot].tic[0], 0).vk;
            if (!format || !prepare_storage_image(&command->image_target[1][slot], format)) {
                const NvnTextureTarget* target = &command->image_target[1][slot];
                LOG_LIMIT(8, "nvn-image-storage: unsupported slot=%u tic-format=%x target-format=%x "
                             "storage-format=%x type=%u storage-type=%u size=%ux%ux%u levels=%u samples=%u "
                             "view=%u:%u:%u:%u flags=%x\n",
                          slot, command->image_descriptor[1][slot].tic[0] & 0x7f, target->format,
                          target->storage_format, target->target, target->storage_target, target->width,
                          target->height, target->depth, target->levels, target->samples, target->view.base_level,
                          target->view.level_count, target->view.first_layer, target->view.layer_count,
                          target->view.flags);
                image_storage_ready = 0;
                break;
            }
        }
    }
    int textureless_shader = shader_ready && !vertex_shader->si.ntex && !fragment_shader->si.ntex;
    u32 texture_slots[NVN_DRAW_SHADER_STAGES];
    memcpy(texture_slots, command->texture_bound, sizeof texture_slots);
    int slots_mapped = shader_ready;
    if (shader_ready) {
        VkSh* shaders[2] = {vertex_shader, fragment_shader};
        memset(texture_slots, 0, sizeof texture_slots);
        /* NVN's generated texture CBUF places binding slot n at word 8 + 2*n. Check that every translated
           texture access fits this layout before narrowing preflight to shader-visible bindings. Bindless and
           unfamiliar offsets retain the conservative all-bound-slot path. This is resource preparation only;
           host draws still need descriptor binding and command-stream ordering. */
        for (u32 stage = 0; stage < 2 && slots_mapped; stage++)
            for (int i = 0; i < shaders[stage]->si.ntex; i++) {
                const ShTex* tex = &shaders[stage]->si.tex[i];
                u32 word = tex->cbuf_off;
                if (tex->bindless || word < 8 || ((word - 8) & 1) || (word - 8) / 2 >= NVN_DRAW_TEXTURE_SLOTS) {
                    slots_mapped = 0;
                    break;
                }
                texture_slots[stage] |= 1u << ((word - 8) / 2);
            }
        if (!slots_mapped) memcpy(texture_slots, command->texture_bound, sizeof texture_slots);
    }
    int sampler_status = 0, has_sampler = 0;
    for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES && !sampler_status && !textureless_shader; stage++)
        for (u32 i = 0; i < NVN_DRAW_TEXTURE_SLOTS; i++) {
            if (!(texture_slots[stage] & (1u << i))) continue;
            has_sampler = 1;
            const NvnSamplerDescriptorSnapshot* sampler = &command->sampler_descriptor[stage][i];
            if (!(command->sampler_descriptor_valid[stage] & (1u << i)) || !sampler->tsc_valid) {
                sampler_status = NVN_DRAW_SAMPLER_MISSING;
                break;
            }
            if (!vk_sampler(sampler->tsc)) {
                sampler_status = NVN_DRAW_SAMPLER_FAILED;
                break;
            }
        }
    if (has_sampler && !sampler_status) sampler_status = NVN_DRAW_SAMPLER_READY;
    int texture_status = 0, has_texture = 0;
    for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES && !texture_status && !textureless_shader; stage++)
        for (u32 i = 0; i < NVN_DRAW_TEXTURE_SLOTS; i++) {
            if (!(texture_slots[stage] & (1u << i))) continue;
            has_texture = 1;
            const NvnDescriptorSnapshot* texture = &command->texture_descriptor[stage][i];
            if (!(command->texture_descriptor_valid[stage] & (1u << i)) || !texture->tic_valid) {
                texture_status = NVN_DRAW_TEXTURE_HEADER_MISSING;
                break;
            }
            if (!vk_tex_tic_supported(texture->tic)) {
                texture_status = NVN_DRAW_TEXTURE_HEADER_UNSUPPORTED;
                break;
            }
        }
    if (has_texture && !texture_status) texture_status = NVN_DRAW_TEXTURE_HEADER_READY;
    if (command->load_textures && texture_status == NVN_DRAW_TEXTURE_HEADER_READY) {
        int ready = 1, failure = 0, from_clear = 0;
        /* This callback runs between queued GPU jobs, before an HLE draw would allocate descriptor sets. The
           existing loader may land pending GPU writes, so it must run before draw setup rather than from it. */
        for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES && ready; stage++)
            for (u32 i = 0; i < NVN_DRAW_TEXTURE_SLOTS; i++) {
                if (!(texture_slots[stage] & (1u << i))) continue;
                const NvnDescriptorSnapshot* descriptor = &command->texture_descriptor[stage][i];
                NvnPoolSnapshot backing = {0};
                /* The hardware TIC outlives SDK texture-object copies and finalization. Keep its captured backing
                   pool generation, then let the TIC/GMMU loader validate the mapped bytes at draw preparation. */
                NvnTextureSnapshot external = {0};
                if ((descriptor->memory_pool &&
                     (!nvn_pool_snapshot(descriptor->memory_pool, &backing) ||
                      backing.generation != descriptor->memory_pool_generation)) ||
                    (!descriptor->memory_pool &&
                     (!nvn_texture_snapshot(descriptor->texture, &external) ||
                      external.generation != descriptor->texture_generation))) {
                    failure = NVN_DRAW_TEXTURE_IMAGE_STALE;
                    ready = 0;
                    break;
                }
                VkImageView view = image_view_from_semantic(descriptor, command->last_unmodeled_write);
                if (view) {
                    from_clear = 1;
                    continue;
                }
                Surf* surf = vk_tex_from_tic(descriptor->tic);
                if (!surf || (surf->f.depth && surf->f.stencil && !surf->is_rt)) {
                    failure = !surf ? NVN_DRAW_TEXTURE_IMAGE_MISSING : NVN_DRAW_TEXTURE_IMAGE_DEPTH;
                    if (failure == NVN_DRAW_TEXTURE_IMAGE_DEPTH) {
                        const u32* tic = command->texture_descriptor[stage][i].tic;
                        LOG_LIMIT(
                            1,
                            "nvn-texture-depth-memory: tic-format=%x vk-format=%u width=%u height=%u levels=%u address=%llx\n",
                            tic[0] & 0x7f, surf->f.vk, (tic[4] & 0xffff) + 1, (tic[5] & 0xffff) + 1,
                            ((tic[3] >> 28) & 15) + 1, (unsigned long long)surf->gva);
                    }
                    ready = 0;
                    break;
                }
            }
        texture_status |= ready ? NVN_DRAW_TEXTURE_IMAGE_READY | (from_clear ? NVN_DRAW_TEXTURE_IMAGE_SEMANTIC : 0)
                                : NVN_DRAW_TEXTURE_IMAGE_FAILED | failure;
    }
    int resource_status = sampler_status | texture_status;
    if (image_storage_ready) resource_status |= NVN_DRAW_IMAGE_STORAGE_READY;
    if (textureless_shader) resource_status |= NVN_DRAW_TEXTURE_UNUSED;
    if (slots_mapped && !textureless_shader) resource_status |= NVN_DRAW_TEXTURE_SLOTS_MAPPED;
    if (command->materialize_shaders)
        resource_status |=
            prepare_vertex_formats(command, shader_ready ? vertex_shader->si.in_mask : UINT32_MAX)
                ? NVN_DRAW_VERTEX_FORMAT_READY : NVN_DRAW_VERTEX_FORMAT_FAILED;
    if (command->materialize_shaders) {
        if (shader_ready) {
            resource_status |= NVN_DRAW_SHADER_MODULE_READY;
            int zero_bound = 0;
            for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES; stage++)
                for (u32 i = 0; i < NVN_DRAW_TEXTURE_SLOTS; i++)
                    zero_bound |= (command->texture_bound[stage] & (1u << i)) && !command->texture[stage][i];
            if (zero_bound) {
                int textured = vertex_shader->si.ntex || fragment_shader->si.ntex;
                resource_status |= textured ? NVN_DRAW_ZERO_BIND_TEXTURED : NVN_DRAW_ZERO_BIND_TEXTURELESS;
                if (textured) {
                    int all_bindless = 1;
                    VkSh* shaders[2] = {vertex_shader, fragment_shader};
                    for (u32 stage = 0; stage < 2; stage++)
                        for (int i = 0; i < shaders[stage]->si.ntex; i++)
                            all_bindless &= shaders[stage]->si.tex[i].bindless;
                    if (all_bindless) resource_status |= NVN_DRAW_ZERO_BIND_ALL_BINDLESS;
                }
            }
        } else {
            resource_status |= NVN_DRAW_SHADER_MODULE_FAILED;
        }
    }
    int has_target = command->color_target_mask || command->depth_target_bound;
    if (!has_target || !command->target_complete) {
        if (command->prepare_pipeline) resource_status |= NVN_DRAW_PIPELINE_FAILED;
        return NVN_DRAW_PREPARED | resource_status;
    }
    if (command->prepare_attachments) {
        int ready = 1;
        for (u32 i = 0; i < 8; i++)
            if ((command->color_target_mask & (1u << i)) && !prepare_attachment(&command->color_target[i], 0))
                ready = 0;
        if (command->depth_target_bound && !prepare_attachment(&command->depth_target, 1)) ready = 0;
        resource_status |= ready ? NVN_DRAW_ATTACHMENT_READY : NVN_DRAW_ATTACHMENT_FAILED;
    }
    if (command->prepare_pipeline)
        resource_status |= (resource_status & NVN_DRAW_SHADER_MODULE_READY) &&
                                   (resource_status & NVN_DRAW_VERTEX_FORMAT_READY) &&
                                   (resource_status & NVN_DRAW_ATTACHMENT_READY) &&
                                   prepare_draw_pipeline(command, vertex_shader, fragment_shader)
                               ? NVN_DRAW_PIPELINE_READY
                               : NVN_DRAW_PIPELINE_FAILED;
    for (u32 i = 0; i < 8; i++) {
        if (!(command->color_target_mask & (1u << i))) continue;
        const NvnTextureTarget* target = &command->color_target[i];
        const Surf* surf = vk_rt_find(target->gpu_address);
        VkFmt format = nvn_format(target->format);
        if (!surf) return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_TARGET_NO_SURFACE;
        if (!format.vk || surf->f.vk != format.vk) {
            LOG_LIMIT(8, "nvn-draw-target-format: color nvn=%x vk=%u surf-vk=%u surf-key=%x\n", target->format,
                      format.vk, surf->f.vk, surf->fkey);
            return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_TARGET_FORMAT;
        }
        if (surf->w != target->width || surf->h != target->height) {
            LOG_LIMIT(8, "nvn-draw-target-size: color nvn=%ux%u surf=%ux%u\n", target->width, target->height, surf->w,
                      surf->h);
            return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_TARGET_SIZE;
        }
        if (target->view.base_level || target->view.first_layer) {
            LOG_LIMIT(8, "nvn-draw-target-view: color level=%u layer=%u\n", target->view.base_level,
                      target->view.first_layer);
            return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_TARGET_VIEW;
        }
    }
    if (command->depth_target_bound) {
        const NvnTextureTarget* target = &command->depth_target;
        const Surf* surf = vk_rt_find(target->gpu_address);
        VkFmt format = nvn_format(target->format);
        if (!surf) return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_TARGET_NO_SURFACE;
        if (!format.vk || surf->f.vk != format.vk) {
            LOG_LIMIT(8, "nvn-draw-target-format: depth nvn=%x vk=%u surf-vk=%u surf-key=%x\n", target->format,
                      format.vk, surf->f.vk, surf->fkey);
            return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_TARGET_FORMAT;
        }
        if (surf->w != target->width || surf->h != target->height) {
            LOG_LIMIT(8, "nvn-draw-target-size: depth nvn=%ux%u surf=%ux%u\n", target->width, target->height, surf->w,
                      surf->h);
            return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_TARGET_SIZE;
        }
        if (target->view.base_level || target->view.first_layer) {
            LOG_LIMIT(8, "nvn-draw-target-view: depth level=%u layer=%u\n", target->view.base_level,
                      target->view.first_layer);
            return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_TARGET_VIEW;
        }
    }
    return NVN_DRAW_PREPARED | resource_status | NVN_DRAW_SHARED_TARGET;
}
