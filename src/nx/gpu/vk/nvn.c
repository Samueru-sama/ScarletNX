/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN-owned probe images. They remain separate from guest Maxwell surfaces until whole-list replacement and
   guest-memory coherence are implemented. */
#include "gpu/vk/vk.h"
#include "gpu/gpu.h"
#include "gpu/nvn_command_gap.h"
#include "gpu/nvn_pool.h"
#include "gpu/nvn_shader.h"
#include "gpu/shader/shader.h"
#include "hle/services/nvdrv.h"
#include "runtime/platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NVN_IMAGE_SLOTS  128
#define NVN_IMAGE_BUDGET (256ull << 20)
#define NVN_SHADOW_STORAGE_SLOTS 128
#define NVN_SHADOW_STORAGE_BUDGET (64ull << 20)
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
    u32 storage_size;
    u64 last_batch;
    u64 shadow_draw_stamp;
    u64 guest_gpu_generation, guest_checked_batch;
    u32 guest_write_clock;
    const u8* guest_host;
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
    int transfer_src;
} NvnImage;
static NvnImage images[NVN_IMAGE_SLOTS];
typedef struct {
    u64 address, size, gpu_generation, checked_batch;
    u32 cpu_clock;
    const u8* guest_host;
    VkBuffer buffer;
    VmaAllocation allocation;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    u8 initial_prefix[64];
    int invalid;
} NvnShadowStorage;
static NvnShadowStorage shadow_storage[NVN_SHADOW_STORAGE_SLOTS];
static u64 shadow_storage_bytes;
static u64 image_bytes;
static u64 batch_serial;
static u64 shadow_draw_stamp;
static u64 modeled_unknown_marker, modeled_unknown_prior;

static int cross_batch_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* opt = getenv("SNX_NVN_CROSS_BATCH");
        enabled = opt && !strcmp(opt, "1");
    }
    return enabled;
}

static u64 effective_unknown_write(u64 marker) {
    return marker == modeled_unknown_marker ? modeled_unknown_prior : marker;
}

static void mark_modeled_unknown(u64 marker, u64 prior) {
    if (!marker) return;
    modeled_unknown_prior = effective_unknown_write(prior);
    modeled_unknown_marker = marker;
}

static void shadow_record_backing(NvnImage* image) {
    if (!cross_batch_enabled()) return;
    image->guest_host = gmem_ptr_untracked(image->address, image->storage_size);
    image->guest_checked_batch = batch_serial;
    if (!image->guest_host || vk_written_since(image->guest_host, image->storage_size, vk_ww_now()) < 0) {
        image->guest_host = NULL;
        return;
    }
    image->guest_write_clock = vk_ww_now();
    image->guest_gpu_generation = gpu_mem_generation();
}

static void shadow_check_backing(NvnImage* image) {
    if (!cross_batch_enabled()) return;
    if (image->guest_checked_batch == batch_serial) return;
    image->guest_checked_batch = batch_serial;
    const u8* host = gmem_ptr_untracked(image->address, image->storage_size);
    int gpu_dirty = image->guest_host && host == image->guest_host &&
                    gpu_mem_dirty_since(image->guest_gpu_generation, image->address, image->storage_size);
    int cpu_dirty = image->guest_host && host == image->guest_host && !gpu_dirty &&
                    vk_written_since(host, image->storage_size, image->guest_write_clock) != 0;
    if (!image->guest_host || host != image->guest_host || gpu_dirty || cpu_dirty) {
        memset(image->color_layers, 0, sizeof image->color_layers);
        memset(image->depth_layers, 0, sizeof image->depth_layers);
        image->guest_host = NULL;
        return;
    }
    image->guest_write_clock = vk_ww_now();
    image->guest_gpu_generation = gpu_mem_generation();
}

void vk_nvn_batch_begin(void) {
    batch_serial++;
    if (cross_batch_enabled()) return;
    modeled_unknown_marker = modeled_unknown_prior = 0;
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
        /* Distinct NVNtexture objects can alias the same pool bytes. Key the host image by the backing
           allocation and compatible shape so a clear through one object is visible through another. */
        if (image->pool == target->pool && image->address == target->gpu_address &&
            image->pool_generation == target->pool_generation && image->width == target->width &&
            image->height == target->height && image->levels == target->levels && image->layers == target->depth &&
            image->target == target->storage_target && image->format == target->storage_format) {
            if (target->storage_size > image->storage_size) {
                image->storage_size = target->storage_size;
                image->guest_host = NULL;
            }
            shadow_check_backing(image);
            return image;
        }
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
    int transfer_src = !!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
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
                              .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       (transfer_src ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0) |
                                       (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
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
    empty->storage_size = target->storage_size;
    image_bytes += bytes;
    empty->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    empty->stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    empty->sampled = sampled;
    empty->attachment = attachment;
    empty->storage = storage;
    empty->transfer_src = transfer_src;
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
    u32 mip_width = mip < 32 ? target->width >> mip : 0;
    u32 mip_height = mip < 32 ? target->height >> mip : 0;
    if (!mip_width) mip_width = 1;
    if (!mip_height) mip_height = 1;
    int partial = 0;
    if (command->region_known) {
        if (command->region[0] < 0 || command->region[1] < 0 || command->region[2] ||
            command->region[3] <= 0 || command->region[4] <= 0 || command->region[5] != 1 ||
            (u32)command->region[0] > mip_width || (u32)command->region[1] > mip_height ||
            (u32)command->region[3] > mip_width - (u32)command->region[0] ||
            (u32)command->region[4] > mip_height - (u32)command->region[1])
            return 0;
        partial = command->region[0] || command->region[1] ||
                  command->region[3] != (i32)mip_width || command->region[4] != (i32)mip_height;
    }
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
        if (partial) return 0;
        if (!format.depth ||
            (command->stencil_mask && command->stencil_mask != 0xff && command->stencil_mask != 0xffffffffu) ||
            (!command->depth_mask && !command->stencil_mask) || (command->stencil_mask && !format.stencil))
            return 0;
    } else {
        return 0;
    }
    VkImageAspectFlags aspect = vk_aspect(&format);
    int full = 0;
    if (partial && !prepare_attachment(target, 0)) return 0;
    NvnImage* image = image_for(target, format.vk, aspect, &full);
    if (!image) {
        LOG_LIMIT(12, "nvn-clear-image-reject: format=%x %ux%ux%u levels=%u cache-full=%d\n", target->format,
                  target->width, target->height, target->depth, target->levels, full);
        return full ? -1 : 0;
    }
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    if (partial) {
        VkImageView view = VK_NULL_HANDLE;
        for (u32 i = 0; i < 16; i++)
            if (image->attachments[i].view && image->attachments[i].format == format.vk &&
                image->attachments[i].mip == mip && image->attachments[i].layer == layer) {
                view = image->attachments[i].view;
                break;
            }
        if (!view) return 0;
        u64 prior = effective_unknown_write(command->prior_unmodeled_write);
        int covered = layer < 64 && (image->color_layers[mip] & (1ull << layer)) &&
                      image->subresource_sequence[mip][layer] > prior;
        vk_barrier(cb, image->image, aspect, image->stage, image->access, image->layout,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        image->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image->stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        image->access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderingAttachmentInfo attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                .imageView = view,
                                                .imageLayout = image->layout,
                                                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                     .renderArea = {{0, 0}, {mip_width, mip_height}},
                                     .layerCount = 1,
                                     .colorAttachmentCount = 1,
                                     .pColorAttachments = &attachment};
        VkClearAttachment clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0};
        memcpy(clear.clearValue.color.float32, command->color, sizeof command->color);
        VkClearRect rect = {.rect = {{command->region[0], command->region[1]},
                                     {(u32)command->region[3], (u32)command->region[4]}},
                            .layerCount = 1};
        vkCmdBeginRendering(cb, &rendering);
        vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
        vkCmdEndRendering(cb);
        if (covered) image->subresource_sequence[mip][layer] = command->sequence;
        image->last_batch = batch_serial;
        mark_modeled_unknown(command->unknown_marker, command->prior_unmodeled_write);
        static u64 partial_clears;
        partial_clears++;
        if (partial_clears == 1 || !(partial_clears % 100))
            LOG("nvn-clear-partial: issued=%llu target=%llx mip=%u layer=%u covered=%d\n",
                (unsigned long long)partial_clears, (unsigned long long)target->gpu_address, mip, layer, covered);
        return 1;
    }
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
    mark_modeled_unknown(command->unknown_marker, command->prior_unmodeled_write);
    shadow_record_backing(image);
    return 1;
}

static int shadow_range_known(const NvnTextureTarget* target) {
    return target->gpu_address && target->storage_size &&
           target->gpu_address <= UINT64_MAX - target->storage_size;
}

static void shadow_invalidate_backing(const NvnTextureTarget* target) {
    u64 end = target->gpu_address + target->storage_size;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        NvnImage* image = &images[i];
        if (!image->image) continue;
        if (image->storage_size && image->address <= UINT64_MAX - image->storage_size &&
            (image->address >= end || target->gpu_address >= image->address + image->storage_size))
            continue;
        memset(image->color_layers, 0, sizeof image->color_layers);
        memset(image->depth_layers, 0, sizeof image->depth_layers);
        image->guest_host = NULL;
    }
}

static int shadow_copy_region_valid(const NvnTextureTarget* target, const i32 region[6]) {
    return target->target == 1 && target->storage_target == 1 && target->format == target->storage_format &&
           target->depth == 1 && target->levels && target->samples <= 1 &&
           !target->view.base_level && !target->view.first_layer &&
           target->view.level_count <= 1 && target->view.layer_count <= 1 &&
           !(target->view.flags & (8 | 16 | 4 | 32)) &&
           region[0] >= 0 && region[1] >= 0 && region[2] == 0 &&
           region[3] > 0 && region[4] > 0 && region[5] == 1 &&
           (u32)region[0] <= target->width && (u32)region[1] <= target->height &&
           (u32)region[3] <= target->width - (u32)region[0] &&
           (u32)region[4] <= target->height - (u32)region[1];
}

static int shadow_blit_texture(const NvnCopyBufferTextureCommand* command) {
    const NvnTextureTarget* source_target = &command->source_target;
    const NvnTextureTarget* target = &command->target;
    if (!shadow_range_known(source_target) || !shadow_copy_region_valid(source_target, command->source_region) ||
        !shadow_copy_region_valid(target, command->target_region) ||
        source_target->format != target->format)
        return 0;
    u64 source_end = source_target->gpu_address + source_target->storage_size;
    u64 target_end = target->gpu_address + target->storage_size;
    if (source_target->gpu_address < target_end && target->gpu_address < source_end) return 0;
    VkFmt format = nvn_format(source_target->format);
    if (!format.vk || format.conv || format.bw > 1 || format.bh > 1) return 0;
    VkFormatProperties properties = {0};
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, format.vk, &properties);
    if ((properties.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) !=
        (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;
    VkImageAspectFlags aspect = vk_aspect(&format);
    int full = 0;
    NvnImage* source = image_for(source_target, format.vk, aspect, &full);
    if (!source || !source->transfer_src ||
        !((format.depth ? source->depth_layers[0] : source->color_layers[0]) & 1) ||
        source->subresource_sequence[0][0] <= effective_unknown_write(command->prior_unmodeled_write))
        return 0;
    /* Keep the source resident while image_for allocates the destination. */
    source->last_batch = batch_serial;
    NvnImage* destination = image_for(target, format.vk, aspect, &full);
    if (!destination || source == destination) return 0;
    int destination_covered = ((format.depth ? destination->depth_layers[0] : destination->color_layers[0]) & 1) &&
                              destination->subresource_sequence[0][0] >
                                  effective_unknown_write(command->prior_unmodeled_write);
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_barrier(cb, source->image, aspect, source->stage, source->access, source->layout,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_barrier(cb, destination->image, aspect, destination->stage, destination->access, destination->layout,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source->stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    source->access = VK_ACCESS_2_TRANSFER_READ_BIT;
    destination->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    destination->stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    destination->access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkImageBlit blit = {.srcSubresource = {format.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, 0, 1},
                        .dstSubresource = {format.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                           0, 0, 1},
                        .srcOffsets = {{command->source_region[0], command->source_region[1], 0},
                                       {command->source_region[0] + command->source_region[3],
                                        command->source_region[1] + command->source_region[4], 1}},
                        .dstOffsets = {{command->target_region[0], command->target_region[1], 0},
                                       {command->target_region[0] + command->target_region[3],
                                        command->target_region[1] + command->target_region[4], 1}}};
    vkCmdBlitImage(cb, source->image, source->layout, destination->image, destination->layout, 1, &blit,
                   VK_FILTER_NEAREST);
    if (format.stencil) {
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        vkCmdBlitImage(cb, source->image, source->layout, destination->image, destination->layout, 1, &blit,
                       VK_FILTER_NEAREST);
    }
    shadow_invalidate_backing(target);
    int full_target = !command->target_region[0] && !command->target_region[1] &&
                      (u32)command->target_region[3] == target->width &&
                      (u32)command->target_region[4] == target->height;
    if (full_target || destination_covered) {
        if (format.depth)
            destination->depth_layers[0] |= 1;
        else
            destination->color_layers[0] |= 1;
        destination->subresource_sequence[0][0] = command->sequence;
        shadow_record_backing(destination);
    }
    source->last_batch = destination->last_batch = batch_serial;
    return 1;
}

int vk_nvn_copy_buffer_texture(const NvnCopyBufferTextureCommand* command) {
    const NvnTextureTarget* target = &command->target;
    if (command->scoped_texture_write) {
        if (!shadow_range_known(target)) return 0;
        if (command->texture_blit && shadow_blit_texture(command)) {
            mark_modeled_unknown(command->unknown_marker, command->prior_unmodeled_write);
            return 2;
        }
        shadow_invalidate_backing(target);
        mark_modeled_unknown(command->unknown_marker, command->prior_unmodeled_write);
        return 1;
    }
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
    shadow_record_backing(image);
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
                   VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image->stage = VK_SAMPLED_STAGES;
        image->access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        return sampled_view;
    }
    return VK_NULL_HANDLE;
}

/* Snapshot a bound attachment before a draw samples and writes it. The descriptor sees the
   preceding contents without asking Vulkan to sample the live writable attachment. */
static VkImageView shadow_attachment_feedback(const NvnTextureTarget* target, u64 prior_unmodeled_write,
                                              int depth, const u32 tic[8]) {
    VkFmt format = nvn_format(target->format);
    if (!!format.depth != !!depth || target->target != 1 || target->storage_target != 1 ||
        target->depth != 1 || target->levels != 1 || target->samples > 1) return VK_NULL_HANDLE;
    int full = 0;
    VkImageAspectFlags aspect = depth ? vk_aspect(&format) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags copy_aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    NvnImage* source = image_for(target, format.vk, aspect, &full);
    if (!source || !source->transfer_src ||
        !((depth ? source->depth_layers[0] : source->color_layers[0]) & 1) ||
        source->subresource_sequence[0][0] <= effective_unknown_write(prior_unmodeled_write))
        return VK_NULL_HANDLE;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format.vk,
                              .extent = {target->width, target->height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
    VmaAllocationCreateInfo allocation = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage snapshot = VK_NULL_HANDLE;
    VmaAllocation memory = NULL;
    if (vmaCreateImage(g_vk.vma, &info, &allocation, &snapshot, &memory, NULL) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    static const VkComponentSwizzle swizzle[8] = {
        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
        VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE};
    VkComponentSwizzle components[4] = {depth ? VK_COMPONENT_SWIZZLE_R : swizzle[(tic[0] >> 19) & 7],
                                        depth ? VK_COMPONENT_SWIZZLE_R : swizzle[(tic[0] >> 22) & 7],
                                        depth ? VK_COMPONENT_SWIZZLE_R : swizzle[(tic[0] >> 25) & 7],
                                        depth ? VK_COMPONENT_SWIZZLE_ONE : swizzle[(tic[0] >> 28) & 7]};
    if (format.bgra)
        for (u32 i = 0; i < 4; i++)
            components[i] = components[i] == VK_COMPONENT_SWIZZLE_R ? VK_COMPONENT_SWIZZLE_B
                          : components[i] == VK_COMPONENT_SWIZZLE_B ? VK_COMPONENT_SWIZZLE_R
                                                                     : components[i];
    VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = snapshot,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = format.vk,
                                       .components = {components[0], components[1], components[2], components[3]},
                                       .subresourceRange = {copy_aspect, 0, 1, 0, 1}};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.dev, &view_info, NULL, &view) != VK_SUCCESS) {
        vk_defer_image(snapshot, memory, VK_NULL_HANDLE, VK_NULL_HANDLE);
        return VK_NULL_HANDLE;
    }
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    vk_barrier(cb, source->image, aspect, source->stage, source->access, source->layout,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_barrier(cb, snapshot, aspect, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region = {.srcSubresource = {copy_aspect, 0, 0, 1},
                          .dstSubresource = {copy_aspect, 0, 0, 1},
                          .extent = {target->width, target->height, 1}};
    vkCmdCopyImage(cb, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snapshot,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vk_barrier(cb, snapshot, aspect, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    source->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source->stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    source->access = VK_ACCESS_2_TRANSFER_READ_BIT;
    source->last_batch = batch_serial;
    vk_defer_image(snapshot, memory, view, VK_NULL_HANDLE);
    return view;
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

enum { NVN_HANDLE_BANK_WORDS = 72 + 2 * NVN_DRAW_IMAGE_SLOTS };

static void build_handle_bank(const NvnDrawCommand* command, u32 stage, u32* words, u8* known) {
    memset(words, 0, NVN_HANDLE_BANK_WORDS * sizeof *words);
    memset(known, 0, NVN_HANDLE_BANK_WORDS * sizeof *known);
    /* The SDK writes texture handles at words 8, 10, ... and image handles at 72, 74, ... in CBUF 2.
       Only the low handle words have been verified against the generated Maxwell bank. */
    for (u32 i = 0; i < NVN_DRAW_TEXTURE_SLOTS; i++)
        if (command->texture_bound[stage] & (1u << i)) {
            words[8 + 2 * i] = (u32)command->texture[stage][i];
            known[8 + 2 * i] = 1;
        }
    for (u32 i = 0; i < NVN_DRAW_IMAGE_SLOTS; i++)
        if (command->image_bound[stage] & (1u << i)) {
            words[72 + 2 * i] = (u32)command->image[stage][i];
            known[72 + 2 * i] = 1;
        }
}

/* NVN stores shader-generated CBUF 1 after the code blob, at the next 256-byte boundary. The address is
   relative to the selected shader, not an application uniform slot. A zero blob size (seen in Odyssey) does
   not establish the data offset and remains unavailable until its control metadata is decoded. */
static u64 shader_embedded_bank1(u64 code_address) {
    u32 header[2] = {0};
    gmem_read_code(code_address, header, sizeof header);
    if (header[0] != 0x12345678 || header[1] < 0x80 || header[1] > NVN_SHADER_CODE_MAX + 0x30)
        return 0;
    u64 offset = ((u64)header[1] + 255) & ~255ull;
    u64 address = code_address <= UINT64_MAX - offset ? code_address + offset : 0;
    return address && gmmu_translate(address) ? address : 0;
}

static int shader_bank_range_mapped(u64 address, u32 size) {
    if (!address || !size || address > UINT64_MAX - size) return 0;
    for (u64 cursor = address, end = address + size; cursor < end;) {
        if (!gmmu_translate(cursor)) return 0;
        u64 step = 0x1000 - (cursor & 0xfff);
        cursor += step < end - cursor ? step : end - cursor;
    }
    return 1;
}

static int materialize_draw_shaders(const NvnDrawCommand* command, VkSh** vertex, VkSh** fragment_shader,
                                    u64 embedded_bank1[2]) {
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
        u64 selected_code = stage ? fragment - 0x30 : (vertex_b ? vertex_b : vertex_a) - 0x30;
        embedded_bank1[stage] = shader_embedded_bank1(selected_code);
        u64 saved_cb[18];
        u32 saved_size[18];
        memcpy(saved_cb, g_vk_cb[row], sizeof saved_cb);
        memcpy(saved_size, g_vk_cbsize[row], sizeof saved_size);
        memset(g_vk_cb[row], 0, sizeof saved_cb);
        memset(g_vk_cbsize[row], 0, sizeof saved_size);
        if (embedded_bank1[stage]) {
            g_vk_cb[row][1] = embedded_bank1[stage];
            g_vk_cbsize[row][1] = 65536;
        }
        /* NVN uniform slot 0 starts at Maxwell CBUF bank 3. Bank 1 comes from the selected shader blob;
           banks 0 and 2 require separate SDK state. Application slots must not overwrite those banks. */
        for (u32 i = 0; i < NVN_DRAW_UNIFORM_SLOTS && i + 3 < 18; i++)
            if (command->uniform_bound[stage] & (1u << i)) {
                g_vk_cb[row][i + 3] = command->uniform[stage][i].address;
                g_vk_cbsize[row][i + 3] = (u32)command->uniform[stage][i].size;
            }
        u32 handle_words[NVN_HANDLE_BANK_WORDS];
        u8 handle_known[NVN_HANDLE_BANK_WORDS];
        build_handle_bank(command, stage, handle_words, handle_known);
        vk_shader_set_nvn_handle_bank(handle_words, handle_known, NVN_HANDLE_BANK_WORDS);
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
        vk_shader_set_nvn_handle_bank(NULL, NULL, 0);
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

/* Check that the observed shader, vertex, blend, polygon, and attachment interface
   can form a Vulkan pipeline, and return its key for the opt-in host draw. */
static VkPipeline prepare_draw_pipeline(const NvnDrawCommand* command, const VkSh* vertex,
                                        const VkSh* fragment_shader, PipeKey* out_key) {
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
    if (command->depth_clamp && !g_vk.feat.depthClamp) return 0;
    key.depth_clamp = command->depth_clamp;
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
    VkPipeline pipeline = vk_pipeline_get(&key);
    if (pipeline && out_key) *out_key = key;
    return pipeline;
}

static VkImageView shadow_attachment_view(const NvnImage* image, VkFormat format, u32 mip, u32 layer) {
    for (u32 i = 0; i < 16; i++)
        if (image->attachments[i].view && image->attachments[i].format == format &&
            image->attachments[i].mip == mip && image->attachments[i].layer == layer)
            return image->attachments[i].view;
    return VK_NULL_HANDLE;
}

static VkImageView shadow_storage_view(const NvnImage* image, VkFormat format, u32 mip, u32 layer) {
    for (u32 i = 0; i < 16; i++)
        if (image->storage_views[i].view && image->storage_views[i].format == format &&
            image->storage_views[i].mip == mip && image->storage_views[i].layer == layer)
            return image->storage_views[i].view;
    return VK_NULL_HANDLE;
}

static const NvnStorageBinding* shadow_storage_binding(const NvnDrawCommand* command, u32 stage, u32 offset) {
    const NvnStorageBinding* selected = NULL;
    for (u32 slot = 0; slot < NVN_DRAW_STORAGE_SLOTS; slot++) {
        if (!(command->storage_known[stage] & (1u << slot))) continue;
        const NvnStorageBinding* binding = &command->storage[stage][slot];
        u32 candidate = 0;
        if (!gpu_unique_cb_offset_macro(0xe20, binding->cbuf_word, stage ? 4 : 0, &candidate)) return NULL;
        if (candidate != offset) continue;
        if (selected) return NULL;
        selected = binding;
    }
    return selected;
}

static int shadow_host_overlap(const u8* a, u64 a_size, const u8* b, u64 b_size) {
    uintptr_t first = (uintptr_t)a, second = (uintptr_t)b;
    if (!a || !b || first > UINTPTR_MAX - a_size || second > UINTPTR_MAX - b_size) return 1;
    return first < second + b_size && second < first + a_size;
}

void vk_nvn_shadow_note_guest_write(u64 address, u32 size, u32 changed_bytes, int compute) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* trace = getenv("SNX_NVN_SHADOW_TRACE_GUEST_WRITES");
        enabled = trace && !strcmp(trace, "1");
    }
    if (!enabled || !size) return;
    const u8* contiguous = gmem_ptr_untracked(address, size);
    for (u32 i = 0; i < NVN_SHADOW_STORAGE_SLOTS; i++) {
        const NvnShadowStorage* entry = &shadow_storage[i];
        if (!entry->buffer) continue;
        int overlap = contiguous && shadow_host_overlap(contiguous, size, entry->guest_host, entry->size);
        if (!contiguous)
            for (u64 offset = 0; offset < size && !overlap;) {
                u32 chunk = 4096 - ((address + offset) & 4095);
                if (chunk > size - offset) chunk = (u32)(size - offset);
                const u8* host = gmem_ptr_untracked(address + offset, chunk);
                overlap = host && shadow_host_overlap(host, chunk, entry->guest_host, entry->size);
                offset += chunk;
            }
        if (overlap)
            LOG_LIMIT(32, "nvn-shadow-guest-write: source=%s addr=%llx size=%u changed=%u cached=%llx/%llu batch=%llu\n",
                      compute ? "compute" : "graphics", (unsigned long long)address, size, changed_bytes,
                      (unsigned long long)entry->address, (unsigned long long)entry->size,
                      (unsigned long long)batch_serial);
    }
}

/* Diagnostic coherence proof for mixed HLE/GPFIFO execution. A guest write may be the
   emulated copy of the same draw; keep HLE state only if the complete results agree. */
static int shadow_storage_matches_guest(NvnShadowStorage* entry) {
    u32 cpu_before = vk_ww_now();
    u64 gpu_before = gpu_mem_generation();
    vk_render_end();
    VkBuffer readback = VK_NULL_HANDLE;
    u8* pixels = vk_readback_buffer(entry->size, &readback);
    if (!pixels) return 0;
    VkCommandBuffer cb = vk_cmd();
    VkBufferMemoryBarrier2 before = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                     .srcStageMask = entry->stage,
                                     .srcAccessMask = entry->access,
                                     .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                     .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .buffer = entry->buffer,
                                     .offset = 0,
                                     .size = entry->size};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                   .bufferMemoryBarrierCount = 1,
                                   .pBufferMemoryBarriers = &before};
    vkCmdPipelineBarrier2(cb, &dependency);
    VkBufferCopy copy = {.size = entry->size};
    vkCmdCopyBuffer(cb, entry->buffer, readback, 1, &copy);
    VkMemoryBarrier2 after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                              .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                              .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    dependency = (VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                    .memoryBarrierCount = 1,
                                    .pMemoryBarriers = &after};
    vkCmdPipelineBarrier2(cb, &dependency);
    vk_finish();
    vk_readback_ready();
    entry->stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    entry->access = VK_ACCESS_2_TRANSFER_READ_BIT;
    u64 different = 0, first = entry->size;
    for (u64 i = 0; i < entry->size; i++)
        if (pixels[i] != entry->guest_host[i]) {
            if (first == entry->size) first = i;
            different++;
        }
    int cpu_stable = vk_written_since(entry->guest_host, entry->size, cpu_before) == 0;
    int gpu_stable = !gpu_mem_dirty_since(gpu_before, entry->address, entry->size);
    LOG_LIMIT(8, "nvn-shadow-storage-compare: addr=%llx size=%llu different=%llu first=%llu bytes=%02x/%02x/%02x stable=%d/%d\n",
              (unsigned long long)entry->address, (unsigned long long)entry->size,
              (unsigned long long)different, (unsigned long long)first,
              first < sizeof entry->initial_prefix ? entry->initial_prefix[first] : 0,
              first < entry->size ? pixels[first] : 0,
              first < entry->size ? entry->guest_host[first] : 0, cpu_stable, gpu_stable);
    const char* input_trace = getenv("SNX_NVN_SHADOW_INPUT_TRACE");
    if (entry->size == 58848 && input_trace && !strcmp(input_trace, "1")) {
        u64 hash = 1469598103934665603ull;
        for (u64 i = 0; i < entry->size; i++) hash = (hash ^ pixels[i]) * 1099511628211ull;
        LOG_LIMIT(8, "nvn-shadow-storage-output: addr=%llx hash=%llx bytes29,304,354=%02x,%02x,%02x guest=%02x,%02x,%02x\n",
                  (unsigned long long)entry->address, (unsigned long long)hash,
                  pixels[29], pixels[304], pixels[354],
                  entry->guest_host[29], entry->guest_host[304], entry->guest_host[354]);
    }
    return !different && cpu_stable && gpu_stable;
}

/* Shadow SSBOs never write the guest's backing. Contained physical aliases share one buffer;
   the shader's word map translates their offsets without requiring a Vulkan descriptor offset. */
static int shadow_storage_get(u64 address, u32 size, int writable, const NvnStorageSeed* seed,
                              NvnShadowStorage** out,
                              u64* out_offset) {
    *out = NULL;
    *out_offset = 0;
    if (!address || !size || size > (16u << 20) || address > UINT64_MAX - size) return 0;
    const u8* host = gmem_ptr_untracked(address, size);
    if (!host) return 0;
    if (seed && (seed->address != address || seed->size != size || seed->guest_host != host))
        return 0;
    /* Image registrations can occur after a shadow SSBO was created. Recheck on every bind. */
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        const NvnImage* image = &images[i];
        if (!image->image) continue;
        if (!image->storage_size || image->address > UINT64_MAX - image->storage_size) return 0;
        const u8* image_host = gmem_ptr_untracked(image->address, image->storage_size);
        if (!image_host || shadow_host_overlap(host, size, image_host, image->storage_size)) return 0;
    }
    for (u32 i = 0; i < NVN_SHADOW_STORAGE_SLOTS; i++) {
        NvnShadowStorage* entry = &shadow_storage[i];
        if (!entry->buffer || !shadow_host_overlap(host, size, entry->guest_host, entry->size)) continue;
        uintptr_t start = (uintptr_t)host, base = (uintptr_t)entry->guest_host;
        u64 offset = start >= base ? start - base : UINT64_MAX;
        if (entry->invalid || offset > entry->size || size > entry->size - offset ||
            (offset & 3)) {
            LOG_LIMIT(12, "nvn-shadow-storage-reject: alias-or-invalid addr=%llx size=%u cached=%llx/%llu invalid=%d\n",
                      (unsigned long long)address, size, (unsigned long long)entry->address,
                      (unsigned long long)entry->size, entry->invalid);
            return 0;
        }
        if (entry->checked_batch != batch_serial) {
            entry->checked_batch = batch_serial;
            int gpu_dirty = gpu_mem_dirty_since(entry->gpu_generation, entry->address, entry->size);
            int cpu_dirty = vk_written_since(entry->guest_host, entry->size, entry->cpu_clock);
            const char* verify_guest = getenv("SNX_NVN_SHADOW_VERIFY_GUEST_WRITES");
            if ((gpu_dirty || cpu_dirty) &&
                (!verify_guest || strcmp(verify_guest, "1") || cpu_dirty < 0 ||
                 !shadow_storage_matches_guest(entry))) {
                LOG_LIMIT(12, "nvn-shadow-storage-reject: guest-write addr=%llx size=%u gpu=%d cpu=%d batch=%llu\n",
                          (unsigned long long)address, size, gpu_dirty, cpu_dirty,
                          (unsigned long long)batch_serial);
                entry->invalid = 1;
                return 0;
            }
            if (gpu_dirty || cpu_dirty)
                LOG_LIMIT(8, "nvn-shadow-storage-verified: addr=%llx size=%llu batch=%llu\n",
                          (unsigned long long)entry->address, (unsigned long long)entry->size,
                          (unsigned long long)batch_serial);
            entry->gpu_generation = gpu_mem_generation();
            entry->cpu_clock = vk_ww_now();
        }
        *out = entry;
        *out_offset = offset;
        return 1;
    }
    if (!writable) return 1;
    if (shadow_storage_bytes > NVN_SHADOW_STORAGE_BUDGET - size) return 0;
    NvnShadowStorage* slot = NULL;
    for (u32 i = 0; i < NVN_SHADOW_STORAGE_SLOTS; i++)
        if (!shadow_storage[i].buffer) {
            slot = &shadow_storage[i];
            break;
        }
    if (!slot) return 0;
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                      .size = size,
                                      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo allocation_info = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                                        VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                               .usage = VMA_MEMORY_USAGE_AUTO};
    VmaAllocationInfo mapped = {0};
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = NULL;
    if (vmaCreateBuffer(g_vk.vma, &buffer_info, &allocation_info, &buffer, &allocation, &mapped) != VK_SUCCESS)
        return 0;
    if (!mapped.pMappedData) {
        vk_defer_buffer(buffer, allocation);
        return 0;
    }
    /* Arm page write tracking before recording the clock used by subsequent batches. */
    vk_written_since(host, size, vk_ww_now());
    const u8* initial = seed ? seed->bytes : host;
    memcpy(mapped.pMappedData, initial, size);
    if (vmaFlushAllocation(g_vk.vma, allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
        vk_defer_buffer(buffer, allocation);
        return 0;
    }
    *slot = (NvnShadowStorage){.address = address, .size = size, .guest_host = host,
                               .buffer = buffer, .allocation = allocation,
                               .gpu_generation = gpu_mem_generation(), .checked_batch = batch_serial,
                               .cpu_clock = vk_ww_now(), .stage = VK_PIPELINE_STAGE_2_HOST_BIT,
                               .access = VK_ACCESS_2_HOST_WRITE_BIT};
    memcpy(slot->initial_prefix, initial, size < sizeof slot->initial_prefix ? size : sizeof slot->initial_prefix);
    const char* input_trace = getenv("SNX_NVN_SHADOW_INPUT_TRACE");
    if (size == 58848 && input_trace && !strcmp(input_trace, "1")) {
        u64 hash = 1469598103934665603ull;
        for (u64 i = 0; i < size; i++) hash = (hash ^ initial[i]) * 1099511628211ull;
        LOG_LIMIT(8, "nvn-shadow-storage-input: addr=%llx batch=%llu hash=%llx byte304=%02x\n",
                  (unsigned long long)address, (unsigned long long)batch_serial,
                  (unsigned long long)hash, initial[304]);
    }
    shadow_storage_bytes += size;
    *out = slot;
    return 1;
}

/* Graphics SSBO shaders index through this map. An uncached read-only binding keeps the
   identity mapping; a shadow binding maps its logical words into the shared physical buffer. */
static int shadow_storage_maps(VkDrawStorage* storage, NvnShadowStorage* buffers[2][16],
                               const u64 offsets[2][16]) {
    for (u32 stage = 0; stage < 2; stage++) {
        if (!storage->n[stage]) continue;
        u64 words = 48;
        for (int i = 0; i < storage->n[stage]; i++) {
            if (!buffers[stage][i]) continue;
            u64 offset = offsets[stage][i];
            u64 size = storage->b[stage][i].size;
            if ((offset & 3) || (size & 3) || offset > buffers[stage][i]->size ||
                size > buffers[stage][i]->size - offset ||
                buffers[stage][i]->size > g_vk.props.limits.maxStorageBufferRange)
                return 0;
            words += ((offset & 4095) + size + 4095) >> 12;
        }
        if (words > UINT32_MAX / 4) return 0;
        u32* map = (u32*)vk_stage(words * 4, vk_stage_align(), &storage->map_buf[stage], &storage->map_off[stage]);
        if (!map) return 0;
        storage->map_size[stage] = words * 4;
        for (u32 i = 0; i < 16; i++) {
            map[i] = ~0u;
            map[16 + i] = 0;
            map[32 + i] = 0;
        }
        u32 at = 48;
        for (int i = 0; i < storage->n[stage]; i++) {
            if (!buffers[stage][i]) continue;
            u64 offset = offsets[stage][i];
            u64 size = storage->b[stage][i].size;
            u32 pages = (u32)(((offset & 4095) + size + 4095) >> 12);
            map[i] = at;
            map[16 + i] = (u32)((offset & 4095) >> 2);
            map[32 + i] = (u32)(size >> 2);
            for (u32 page = 0; page < pages; page++) map[at++] = (u32)(offset >> 12) + page;
            if (offset)
                LOG_LIMIT(8, "nvn-shadow-storage-map: stage=%u binding=%d offset=%llu size=%llu pages=%u\n",
                          stage, i, (unsigned long long)offset, (unsigned long long)size, pages);
        }
    }
    return 1;
}

static int shadow_cbuf0_fill(const NvnDrawCommand* command, const ShaderInfo* si, u32 stage,
                             u8* staged, u32 size) {
    if (si->cbuf0_untracked_read) return 0;
    u32 known[13] = {0};
    known[0x640 / 128] |= (1u << ((0x640 / 4) & 31)) | (1u << ((0x644 / 4) & 31));
    for (int i = 0; i < si->ngbuf; i++) {
        u32 off = si->gbuf_cbuf_off[i];
        if (off > size || size - off < 12 || off >= 0x680 || (off & 3)) return 0;
        const NvnStorageBinding* binding = shadow_storage_binding(command, stage, off);
        if (!binding || !binding->range.address || !binding->range.size || binding->range.size > UINT32_MAX)
            return 0;
        u32 words[3] = {(u32)binding->range.address, (u32)(binding->range.address >> 32),
                        (u32)binding->range.size};
        memcpy(staged + off, words, sizeof words);
        for (u32 word = off / 4; word < off / 4 + 3; word++)
            known[word / 32] |= 1u << (word & 31);
    }
    for (u32 i = 0; i < 13; i++)
        if (si->cbuf0_read_words[i] & ~known[i]) return 0;
    return 1;
}

/* Execute textureless NVN compute on the graphics queue so its SSBO writes precede later
   semantic draws in the same batch. The guest QMD still runs for comparison and guest-visible
   memory; this path owns only the shadow copies consumed by direct NVN drawing. */
int vk_nvn_dispatch(const NvnDispatchCommand* command) {
    if (!command->control_known || !command->groups[0] || !command->groups[1] || !command->groups[2] ||
        !command->local[0] || !command->local[1] || !command->local[2]) return 0;
    const VkPhysicalDeviceLimits* limits = &g_vk.props.limits;
    for (u32 axis = 0; axis < 3; axis++)
        if (command->groups[axis] > limits->maxComputeWorkGroupCount[axis] ||
            command->local[axis] > limits->maxComputeWorkGroupSize[axis]) return 0;
    if ((u64)command->local[0] * command->local[1] * command->local[2] >
            limits->maxComputeWorkGroupInvocations ||
        command->shared_size > limits->maxComputeSharedMemorySize) return 0;
    VkPipeline pipeline;
    const ShaderInfo* si;
    if (!vk_nvn_compute_pipeline(command, &pipeline, &si)) return 0;
    LOG_LIMIT(24, "nvn-shadow-dispatch-shader: code=%llx cbuf=%x cb1-offset=%x cb3-max=%u cb4-max=%u buffers=%d writes=%x textures=%d images=%d unsupported=%d\n",
              (unsigned long long)command->program.snapshot.refs[0].code_candidate,
              si->cbuf_mask, command->embedded_cbuf1_offset, si->cbuf_max[3], si->cbuf_max[4],
              si->ngbuf, si->gbuf_wmask, si->ntex, si->nimg, si->unsupported);
    if (si->unsupported || si->ntex > 1 || si->nimg ||
        !(si->cbuf_mask & 1) || (si->cbuf_mask & ~0xfbu) ||
        si->ngbuf < 1 || si->ngbuf > NVN_DRAW_STORAGE_SLOTS ||
        si->cbuf0_untracked_read || !si->gbuf_wmask) {
        LOG_LIMIT(24, "nvn-shadow-dispatch-skip: shader-capabilities code=%llx cbuf0-untracked=%d\n",
                  (unsigned long long)command->program.snapshot.refs[0].code_candidate,
                  si->cbuf0_untracked_read);
        return 0;
    }
    const ShTex* sampled_shader = si->ntex ? &si->tex[0] : NULL;
    if (sampled_shader &&
        (sampled_shader->bindless || sampled_shader->bl2 || sampled_shader->cbuf_off != 8 ||
         sampled_shader->type != 1 || sampled_shader->array || sampled_shader->shadow ||
         !(command->texture_bound & 1) || !command->texture0_descriptor_valid ||
         !command->texture0_sampler_valid || !command->texture0_descriptor.tic_valid ||
         !command->texture0_sampler.tsc_valid || (si->tex_int_mask & 1))) return 0;
    u32 known[13] = {0};
    known[0x200 / 128] |= 7u;
    NvnShadowStorage* buffers[NVN_DRAW_STORAGE_SLOTS] = {0};
    u64 offsets[NVN_DRAW_STORAGE_SLOTS] = {0};
    u32 slots[NVN_DRAW_STORAGE_SLOTS] = {0};
    u32 sizes[NVN_DRAW_STORAGE_SLOTS] = {0};
    VkDescriptorBufferInfo storage_infos[NVN_DRAW_STORAGE_SLOTS] = {0};
    for (int i = 0; i < si->ngbuf; i++) {
        u32 off = si->gbuf_cbuf_off[i];
        /* This read-only descriptor aliases the SDK's dispatch-dimension words. Maxwell's
           QMD resolves it to an unmapped four-byte range, whose reads return zero. */
        u64 dimension_address = ((u64)command->groups[1] << 32) | command->groups[0];
        if (off == 0x200 && !(si->gbuf_wmask & (1u << i)) &&
            command->groups[2] == 1 && !gmmu_translate(dimension_address)) {
            slots[i] = UINT32_MAX;
            continue;
        }
        if (off < 0x310 || off >= 0x410 || ((off - 0x310) & 15)) {
            LOG_LIMIT(24, "nvn-shadow-dispatch-skip: descriptor code=%llx offset=%x\n",
                      (unsigned long long)command->program.snapshot.refs[0].code_candidate, off);
            return 0;
        }
        u32 slot = (off - 0x310) / 16;
        if (!(command->storage_bound & (1u << slot))) {
            LOG_LIMIT(24, "nvn-shadow-dispatch-skip: storage-unbound code=%llx slot=%u bound=%x\n",
                      (unsigned long long)command->program.snapshot.refs[0].code_candidate,
                      slot, command->storage_bound);
            return 0;
        }
        const NvnBufferRange* range = &command->storage[slot];
        if (!range->address || !range->size || range->size > (16u << 20)) return 0;
        u64 size = (range->size + 3) & ~3ull;
        if (size > limits->maxStorageBufferRange) {
            LOG_LIMIT(24, "nvn-shadow-dispatch-skip: storage-resource code=%llx slot=%u addr=%llx size=%llu\n",
                      (unsigned long long)command->program.snapshot.refs[0].code_candidate, slot,
                      (unsigned long long)range->address, (unsigned long long)size);
            return 0;
        }
        slots[i] = slot;
        sizes[i] = (u32)size;
        for (u32 word = off / 4; word < off / 4 + 3; word++) known[word / 32] |= 1u << (word & 31);
    }
    for (u32 i = 0; i < 13; i++)
        if (si->cbuf0_read_words[i] & ~known[i]) {
            LOG_LIMIT(24, "nvn-shadow-dispatch-skip: cbuf0-unknown code=%llx row=%u missing=%08x\n",
                      (unsigned long long)command->program.snapshot.refs[0].code_candidate,
                      i, si->cbuf0_read_words[i] & ~known[i]);
            return 0;
        }
    for (int i = 0; i < si->ngbuf; i++) {
        u32 slot = slots[i];
        if (slot == UINT32_MAX) {
            VkBuffer buffer;
            u64 offset;
            u8* zero = vk_stage(4, vk_stage_align(), &buffer, &offset);
            if (!zero) return 0;
            memset(zero, 0, 4);
            storage_infos[i] = (VkDescriptorBufferInfo){buffer, offset, 4};
            continue;
        }
        const NvnBufferRange* range = &command->storage[slot];
        if (!command->storage_seed[slot] ||
            !shadow_storage_get(range->address, sizes[i], 1, command->storage_seed[slot],
                                &buffers[i], &offsets[i]) || !buffers[i] ||
            offsets[i] % limits->minStorageBufferOffsetAlignment) {
            LOG_LIMIT(24, "nvn-shadow-dispatch-skip: storage-resource code=%llx slot=%u addr=%llx size=%u\n",
                      (unsigned long long)command->program.snapshot.refs[0].code_candidate, slot,
                      (unsigned long long)range->address, sizes[i]);
            return 0;
        }
        storage_infos[i] = (VkDescriptorBufferInfo){buffers[i]->buffer, offsets[i], sizes[i]};
    }
    VkDescriptorSet set = vk_desc_alloc(g_vk_dslayout[shader_vk_set(ST_CS)]);
    if (!set) return 0;
    VkDescriptorBufferInfo uniform_infos[8] = {0};
    VkWriteDescriptorSet writes[8 + NVN_DRAW_STORAGE_SLOTS + 1] = {0};
    u32 nwrites = 0;
    for (u32 bank = 0; bank < 8; bank++) {
        if (!(si->cbuf_mask & (1u << bank))) continue;
        u32 cb_size = 0;
        const u8* cb_data = vk_nvn_compute_cbuf(bank, &cb_size);
        if (!cb_data || !cb_size || cb_size > limits->maxUniformBufferRange) return 0;
        VkBuffer cb_buffer;
        u64 cb_offset;
        u8* staged = vk_stage(cb_size, vk_stage_align(), &cb_buffer, &cb_offset);
        if (!staged) return 0;
        memcpy(staged, cb_data, cb_size);
        uniform_infos[bank] = (VkDescriptorBufferInfo){cb_buffer, cb_offset, cb_size};
        writes[nwrites++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                  .dstSet = set, .dstBinding = SNX_VKB_CB + bank,
                                                  .descriptorCount = 1,
                                                  .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                  .pBufferInfo = &uniform_infos[bank]};
    }
    for (int i = 0; i < si->ngbuf; i++)
        writes[nwrites++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                   .dstSet = set, .dstBinding = SNX_VKB_SSBO + i,
                                                   .descriptorCount = 1,
                                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   .pBufferInfo = &storage_infos[i]};
    VkDescriptorImageInfo sampled_info = {0};
    if (sampled_shader) {
        const NvnDescriptorSnapshot* descriptor = &command->texture0_descriptor;
        const NvnSamplerDescriptorSnapshot* sampler = &command->texture0_sampler;
        vk_render_end();
        VkImageView view = image_view_from_semantic(descriptor,
                                                    effective_unknown_write(command->prior_unmodeled_write));
        if (!view) {
            Surf* sampled = vk_tex_from_tic(descriptor->tic);
            if (!sampled) return 0;
            view = vk_tex_bind_view(sampled, vk_tex_view_type(sampled_shader), descriptor->tic, 0);
            if (view)
                vk_surf_use(vk_cmd(), sampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        VkSampler handle = vk_tex_bind_sampler(sampler->tsc, sampled_shader, 0);
        if (!view || !handle) return 0;
        sampled_info = (VkDescriptorImageInfo){handle, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[nwrites++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                  .dstSet = set, .dstBinding = SNX_VKB_TEX,
                                                  .descriptorCount = 1,
                                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                  .pImageInfo = &sampled_info};
    }
    vkUpdateDescriptorSets(g_vk.dev, nwrites, writes, 0, NULL);
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    VkBufferMemoryBarrier2 barriers[NVN_DRAW_STORAGE_SLOTS] = {0};
    u32 count = 0;
    for (int i = 0; i < si->ngbuf; i++) {
        NvnShadowStorage* entry = buffers[i];
        if (!entry) continue;
        int seen = 0;
        for (u32 j = 0; j < count; j++) seen |= barriers[j].buffer == entry->buffer;
        if (seen) continue;
        barriers[count++] = (VkBufferMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask = entry->stage, .srcAccessMask = entry->access,
            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = entry->buffer, .offset = 0, .size = entry->size};
        entry->stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        entry->access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if (count) {
        VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                       .bufferMemoryBarrierCount = count,
                                       .pBufferMemoryBarriers = barriers};
        vkCmdPipelineBarrier2(cb, &dependency);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk_playout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, command->groups[0], command->groups[1], command->groups[2]);
    const char* input_trace = getenv("SNX_NVN_SHADOW_INPUT_TRACE");
    if (input_trace && !strcmp(input_trace, "1")) {
        static u32 probes;
        for (int i = 0; i < si->ngbuf && probes < 4; i++)
            if (buffers[i] && buffers[i]->size == 58848) {
                LOG("nvn-shadow-storage-probe: code=%llx probe=%u\n",
                    (unsigned long long)command->program.snapshot.refs[0].code_candidate, probes++);
                shadow_storage_matches_guest(buffers[i]);
            }
    }
    mark_modeled_unknown(command->sequence, command->prior_unmodeled_write);
    LOG_LIMIT(12, "nvn-shadow-dispatch: code=%llx groups=%ux%ux%u local=%ux%ux%u buffers=%d\n",
              (unsigned long long)command->program.snapshot.refs[0].code_candidate,
              command->groups[0], command->groups[1], command->groups[2],
              command->local[0], command->local[1], command->local[2], si->ngbuf);
    return 1;
}

/* First executable NVN slice: an isolated draw with application constant buffers and sampled textures.
   It renders to NVN-owned images for comparison; the SDK's GPFIFO still owns presentation. */
static int shadow_draw_simple(const NvnDrawCommand* command, const VkSh* vertex, const VkSh* fragment,
                              VkPipeline pipeline, const PipeKey* key, const u64 embedded_bank1[2]) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* opt = getenv("SNX_NVN_SHADOW_DRAW");
        enabled = opt && !strcmp(opt, "1");
    }
    int indexed_triangles = command->primitive == 4 && command->indexed &&
                            (command->index_type == 1 || command->index_type == 2 ||
                             (command->index_type == 0 && g_vk.has_index_uint8));
    int triangles = command->primitive == 4 && !command->indexed;
    int strip = command->primitive == 5 && !command->indexed;
    u32 cbuf_mask = vertex && fragment ? vertex->si.cbuf_mask | fragment->si.cbuf_mask : 0;
    /* NVNpolygonState stores cull/front-face, polygon mode, then point/line/fill
       polygon-offset enables in its low byte. Other observed bits do not change
       fill rasterization; the actual fill offset is supplied by its own command. */
    int fill_offset = !!(command->polygon_state & 0x80);
    int fill_polygon = ((command->polygon_state >> 3) & 3) == 2 &&
                       !command->rasterizer_discard &&
                       (!fill_offset || (command->polygon_offset_known &&
                                         (g_vk.feat.depthBiasClamp || !command->polygon_offset[2])));
    if (!enabled || !pipeline || !vertex || !fragment ||
        (cbuf_mask & ~((1u << 18) - 1)) ||
        ((cbuf_mask & 1) && (command->first || command->base_vertex || command->base_instance)) ||
        vertex->si.ntex < 0 ||
        vertex->si.ntex > SH_MAXTEX || fragment->si.ntex < 0 || fragment->si.ntex > SH_MAXTEX ||
        vertex->si.nimg || fragment->si.nimg > NVN_DRAW_IMAGE_SLOTS ||
        vertex->si.ngbuf < 0 || vertex->si.ngbuf > 16 ||
        fragment->si.ngbuf < 0 || fragment->si.ngbuf > 16 ||
        !(command->color_target_mask & 1) || !command->target_complete ||
        (!strip && !triangles && !indexed_triangles) || !command->count || !command->instance_count ||
        !fill_polygon ||
        (command->color_state & 0xffff00ffu) != 0x08030000u ||
        (key->blend[0].en != !!(command->color_state & 0x100)) ||
        (command->depth_target_bound &&
         (((command->depth_stencil_state[0] & 1) &&
           (((command->depth_stencil_state[0] >> 4) & 15) < 1 ||
            ((command->depth_stencil_state[0] >> 4) & 15) > 8)) ||
          (command->depth_target.format == 0x35 && (command->depth_stencil_state[0] & 4)) ||
          (command->depth_bounds_enabled &&
           (!command->depth_bounds_known || !g_vk.feat.depthBounds)))) ||
        !(command->viewport_known & 1) ||
        !(command->scissor_known & 1) ||
        (command->color_target[0].format != 0x0a && command->color_target[0].format != 0x25 &&
         command->color_target[0].format != 0x29 && command->color_target[0].format != 0x38 &&
         command->color_target[0].format != 0x3d && command->color_target[0].format != 0x3f) ||
        (command->color_target[0].target != 1 && command->color_target[0].target != 4) ||
        (command->color_target[0].storage_target != 1 && command->color_target[0].storage_target != 4 &&
         command->color_target[0].storage_target != 8) ||
        command->color_target[0].view.base_level >= command->color_target[0].levels ||
        command->color_target[0].view.base_level >= 16 || command->color_target[0].view.first_layer >= 64 ||
        (command->depth_target_bound &&
         ((command->depth_target.format != 0x32 && command->depth_target.format != 0x34 &&
           command->depth_target.format != 0x35) ||
          (command->depth_target.target != 1 && command->depth_target.target != 4) ||
          (command->depth_target.storage_target != 1 && command->depth_target.storage_target != 4 &&
           command->depth_target.storage_target != 8) ||
          command->depth_target.view.base_level >= command->depth_target.levels ||
          command->depth_target.view.base_level >= 16 || command->depth_target.view.first_layer >= 64 ||
          !command->depth_target.width || !command->depth_target.height)) ||
        key->nbind != key->nattr || key->nbind > 32)
        return 0;
    /* The recorder marks guest writes as unknown while GPFIFO remains authoritative. Successful
       shadow clears and draws can bridge their own synthetic markers; any other unknown write breaks
       the chain, so a failed or unsupported operation cannot certify sampled-image contents. */
    u64 effective_unknown = effective_unknown_write(command->last_unmodeled_write);
    const NvnTextureTarget* color_target = &command->color_target[0];
    const NvnTextureTarget* depth_target = &command->depth_target;
    u32 color_mip = color_target->view.base_level;
    u32 color_width = color_target->width >> color_mip;
    u32 color_height = color_target->height >> color_mip;
    if (!color_width) color_width = 1;
    if (!color_height) color_height = 1;
    for (u32 i = 1; i < key->ncolor; i++) {
        if (!(command->color_target_mask & (1u << i))) continue;
        const NvnTextureTarget* target = &command->color_target[i];
        VkFmt format = nvn_format(target->format);
        u32 mip = target->view.base_level;
        u32 width = mip < 16 ? target->width >> mip : 0;
        u32 height = mip < 16 ? target->height >> mip : 0;
        if (!width) width = 1;
        if (!height) height = 1;
        if (!format.vk || format.depth || (target->target != 1 && target->target != 4) ||
            (target->storage_target != 1 && target->storage_target != 4 &&
             target->storage_target != 8) || mip >= target->levels || mip >= 16 ||
            target->view.first_layer >= 64 || width != color_width ||
            height != color_height || target->samples != color_target->samples)
            return 0;
    }
    u32 color_layer = color_target->view.first_layer;
    u32 depth_layer = depth_target->view.first_layer;
    float vp_data[4];
    memcpy(vp_data, command->viewport[0], sizeof vp_data);
    if (!(vp_data[2] > 0 && vp_data[3] > 0) || vp_data[0] < 0 || vp_data[1] < 0 ||
        vp_data[0] + vp_data[2] > color_width || vp_data[1] + vp_data[3] > color_height)
        return 0;
    const u32* sc_data = command->scissor[0];
    if (sc_data[0] > color_width || sc_data[1] > color_height ||
        sc_data[2] > color_width - sc_data[0] ||
        sc_data[3] > color_height - sc_data[1])
        return 0;
    u64 setup_frame = vk_frame_number();
    VkBuffer vertex_buffers[32] = {0};
    VkDeviceSize vertex_offsets[32] = {0}, vertex_strides[32] = {0};
    for (u32 i = 0; i < key->nattr; i++) {
        u32 location = key->attr[i].loc, binding = key->attr[i].binding;
        if (location >= command->vertex_attrib_count || binding >= key->nbind || vertex_buffers[binding]) return 0;
        const NvnVertexAttrib* attrib = &command->vertex_attrib[location];
        u32 stream = attrib->stream;
        if (!attrib->enabled || stream >= command->vertex_stream_count ||
            !(command->vertex_bound & (1u << stream)) ||
            !vk_buf_get_strict(command->vertex[stream].address, command->vertex[stream].size,
                               &vertex_buffers[binding], &vertex_offsets[binding]))
            return 0;
        vertex_strides[binding] = command->vertex_stream[stream].stride;
    }
    for (u32 i = 0; i < key->nbind; i++)
        if (!vertex_buffers[i]) return 0;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    u64 index_offset = 0;
    if (indexed_triangles &&
        !vk_buf_get_strict(command->index_address, (u64)command->count << command->index_type,
                           &index_buffer, &index_offset))
        return 0;
    VkDrawStorage storage = {0};
    NvnShadowStorage* shadow_buffers[2][16] = {0};
    u64 shadow_buffer_offsets[2][16] = {0};
    const VkSh* storage_shaders[2] = {vertex, fragment};
    for (u32 stage = 0; stage < 2; stage++) {
        const ShaderInfo* si = &storage_shaders[stage]->si;
        storage.n[stage] = si->ngbuf;
        for (int i = 0; i < si->ngbuf; i++) {
            const NvnStorageBinding* binding = shadow_storage_binding(command, stage, si->gbuf_cbuf_off[i]);
            if (!binding || !binding->range.address || !binding->range.size ||
                binding->range.size > (16u << 20)) return 0;
            u64 aligned_size = (binding->range.size + 3) & ~3ull;
            if (aligned_size > (16u << 20) || aligned_size > g_vk.props.limits.maxStorageBufferRange ||
                !shadow_storage_get(binding->range.address, (u32)aligned_size,
                                    !!(si->gbuf_wmask & (1u << i)), NULL, &shadow_buffers[stage][i],
                                    &shadow_buffer_offsets[stage][i])) return 0;
            storage.b[stage][i].addr = binding->range.address;
            storage.b[stage][i].size = (u32)aligned_size;
        }
    }
    if (!shadow_storage_maps(&storage, shadow_buffers, shadow_buffer_offsets)) return 0;
    NvnImage* storage_images[NVN_DRAW_IMAGE_SLOTS] = {0};
    VkDescriptorImageInfo storage_infos[NVN_DRAW_IMAGE_SLOTS] = {0};
    for (int k = 0; k < fragment->si.nimg; k++) {
        const u32 word = fragment->si.img[k].cbuf_off;
        if (fragment->si.img[k].type != 3 || !fragment->si.img[k].bindless ||
            fragment->si.img[k].bl_cbuf != 2 ||
            fragment->si.img[k].number_type != 1 || word < 72 || ((word - 72) & 1) ||
            (word - 72) / 2 >= NVN_DRAW_IMAGE_SLOTS)
            return 0;
        u32 slot = (word - 72) / 2;
        if (!(command->image_bound[1] & (1u << slot)) ||
            !(command->image_descriptor_valid[1] & (1u << slot)) ||
            !(command->image_target_valid[1] & (1u << slot))) return 0;
        const NvnDescriptorSnapshot* descriptor = &command->image_descriptor[1][slot];
        const NvnTextureTarget* target = &command->image_target[1][slot];
        int aliases_color = 0;
        for (u32 i = 0; i < key->ncolor; i++)
            aliases_color |= (command->color_target_mask & (1u << i)) &&
                             target->gpu_address == command->color_target[i].gpu_address;
        if (!descriptor->tic_valid || target->view.base_level || target->view.first_layer ||
            aliases_color ||
            (command->depth_target_bound && target->gpu_address == depth_target->gpu_address))
            return 0;
        VkFormat format = vk_tic_format(descriptor->tic[0], 0).vk;
        if (format != VK_FORMAT_R32_UINT || !prepare_storage_image(target, format)) return 0;
        int full = 0;
        NvnImage* image = image_for(target, format, VK_IMAGE_ASPECT_COLOR_BIT, &full);
        if (!image || !image->storage) return 0;
        for (int j = 0; j < k; j++)
            if (storage_images[j] == image) return 0;
        VkImageView view = shadow_storage_view(image, format, 0, 0);
        if (!view) return 0;
        storage_images[k] = image;
        storage_infos[k] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkDescriptorSet sets[2];
    VkDescriptorBufferInfo uniforms[2][18] = {0};
    VkDescriptorBufferInfo storage_buffers[2][16] = {0};
    VkDescriptorBufferInfo storage_maps[2] = {0};
    VkDescriptorImageInfo textures[2][SH_MAXTEX] = {0};
    VkWriteDescriptorSet writes[2 * (18 + 16 + 1 + SH_MAXTEX + NVN_DRAW_IMAGE_SLOTS)] = {0};
    u32 nwrites = 0;
    int semantic_sampled = 0;
    VkImageView color_feedback_view[8] = {0};
    VkImageView depth_feedback_view = VK_NULL_HANDLE;
    const VkSh* shaders[2] = {vertex, fragment};
    for (u32 stage = 0; stage < 2; stage++) {
        sets[stage] = vk_desc_alloc(g_vk_dslayout[stage]);
        if (!sets[stage]) return 0;
        const ShaderInfo* si = &shaders[stage]->si;
        u32 handle_words[NVN_HANDLE_BANK_WORDS];
        u8 handle_known[NVN_HANDLE_BANK_WORDS];
        if (si->cbuf_mask & 4) {
            if (si->cbuf2_untracked_read) return 0;
            build_handle_bank(command, stage, handle_words, handle_known);
            for (u32 word = 0; word < 96; word++)
                if ((si->cbuf2_read_words[word / 32] & (1u << (word & 31))) &&
                    (word >= NVN_HANDLE_BANK_WORDS || !handle_known[word]))
                    return 0;
        }
        for (u32 bank = 0; bank < 18; bank++) {
            if (!(si->cbuf_mask & (1u << bank))) continue;
            u32 slot = bank >= 3 ? bank - 3 : 0;
            if ((bank == 1 && !embedded_bank1[stage]) ||
                (bank >= 3 && !(command->uniform_bound[stage] & (1u << slot)))) return 0;
            u32 size = si->cbuf_max[bank];
            if (size < 16) size = 16;
            if (size > g_vk.props.limits.maxUniformBufferRange || size > 65536) return 0;
            if (bank == 1 && !shader_bank_range_mapped(embedded_bank1[stage], size)) return 0;
            VkBuffer buffer;
            u64 offset;
            u8* staged = vk_stage(size, vk_stage_align(), &buffer, &offset);
            if (!staged) return 0;
            memset(staged, 0, size);
            if (bank == 2)
                for (u32 word = 0; word < NVN_HANDLE_BANK_WORDS && (word + 1) * 4 <= size; word++)
                    if (handle_known[word]) memcpy(staged + word * 4, &handle_words[word], 4);
            /* The observed SDK draw-parameter words are zero when the recorded draw has zero bases.
               Other bank-0 reads remain ineligible until their source is reconstructed. */
            u64 read_size = bank == 0 || bank == 2 ? 0
                            : bank == 1 ? size : command->uniform[stage][slot].size;
            if (read_size > size) read_size = size;
            if (read_size) {
                if (bank >= 3) {
                    const NvnStorageSeed* seed = command->uniform_seed[stage][slot];
                    if (!seed || seed->address != command->uniform[stage][slot].address ||
                        seed->size < read_size) return 0;
                    memcpy(staged, seed->bytes, read_size);
                } else
                    gmem_read(embedded_bank1[stage], staged, read_size);
            }
            if (bank == 0 && !shadow_cbuf0_fill(command, si, stage, staged, size)) return 0;
            uniforms[stage][bank] = (VkDescriptorBufferInfo){buffer, offset, size};
            writes[nwrites++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                      .dstSet = sets[stage],
                                                      .dstBinding = SNX_VKB_CB + bank,
                                                      .descriptorCount = 1,
                                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                      .pBufferInfo = &uniforms[stage][bank]};
        }
        for (int i = 0; i < si->ngbuf; i++) {
            VkBuffer buffer;
            u64 offset;
            VkDrawGbuf* binding = &storage.b[stage][i];
            if (shadow_buffers[stage][i]) {
                buffer = shadow_buffers[stage][i]->buffer;
                offset = 0;
            } else if (!vk_buf_get_strict(binding->addr, binding->size, &buffer, &offset)) return 0;
            storage_buffers[stage][i] = (VkDescriptorBufferInfo){
                buffer, offset, shadow_buffers[stage][i] ? shadow_buffers[stage][i]->size : binding->size};
            writes[nwrites++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                      .dstSet = sets[stage],
                                                      .dstBinding = SNX_VKB_SSBO + i,
                                                      .descriptorCount = 1,
                                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                      .pBufferInfo = &storage_buffers[stage][i]};
        }
        if (si->ngbuf) {
            storage_maps[stage] =
                (VkDescriptorBufferInfo){storage.map_buf[stage], storage.map_off[stage], storage.map_size[stage]};
            writes[nwrites++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                      .dstSet = sets[stage],
                                                      .dstBinding = SNX_VKB_SMAP,
                                                      .descriptorCount = 1,
                                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                      .pBufferInfo = &storage_maps[stage]};
        }
        for (int tex_index = 0; tex_index < si->ntex; tex_index++) {
            const ShTex* shader_texture = &si->tex[tex_index];
            u32 word = shader_texture->cbuf_off;
            if (shader_texture->bindless || shader_texture->bl2 || word < 8 ||
                ((word - 8) & 1) || (word - 8) / 2 >= NVN_DRAW_TEXTURE_SLOTS) return 0;
            u32 slot = (word - 8) / 2;
            if (!(command->texture_bound[stage] & (1u << slot)) ||
                !(command->texture_descriptor_valid[stage] & (1u << slot)) ||
                !(command->sampler_descriptor_valid[stage] & (1u << slot))) return 0;
            const NvnDescriptorSnapshot* descriptor = &command->texture_descriptor[stage][slot];
            const NvnSamplerDescriptorSnapshot* sampler = &command->sampler_descriptor[stage][slot];
            if (!descriptor->tic_valid || !sampler->tsc_valid) return 0;
            int num = shader_texture->shadow || !(si->tex_int_mask & (1u << tex_index)) ? 0
                      : (si->tex_sint_mask & (1u << tex_index)) ? 2 : 1;
            VkImageView view = VK_NULL_HANDLE;
            u64 sampled_address = 0;
            u32 sampled_width = 0, sampled_height = 0, sampled_layers = 0, sampled_levels = 0;
            VkImageViewType sampled_type = 0;
            VkFormat sampled_format = VK_FORMAT_UNDEFINED;
            int semantic_shape = vk_tex_tic_shadow(descriptor->tic, &sampled_address, &sampled_width,
                                                   &sampled_height, &sampled_layers, &sampled_levels,
                                                   &sampled_type, &sampled_format);
            int color_feedback_index = -1;
            if (semantic_shape)
                for (u32 i = 0; i < key->ncolor; i++)
                    if ((command->color_target_mask & (1u << i)) &&
                        sampled_address == command->color_target[i].gpu_address) {
                        color_feedback_index = (int)i;
                        break;
                    }
            int color_feedback = color_feedback_index >= 0;
            int depth_feedback = semantic_shape && command->depth_target_bound &&
                                 sampled_address == depth_target->gpu_address;
            if (semantic_shape)
                for (int k = 0; k < fragment->si.nimg; k++)
                    if (sampled_address == storage_images[k]->address) return 0;
            /* Semantic views currently have identity component mapping. Other swizzles require views. */
            int identity_swizzle = ((descriptor->tic[0] >> 19) & 7) == 2 &&
                                   ((descriptor->tic[0] >> 22) & 7) == 3 &&
                                   ((descriptor->tic[0] >> 25) & 7) == 4 &&
                                   ((descriptor->tic[0] >> 28) & 7) == 5;
            /* G24R8's G component is depth; a depth-aspect Vulkan view exposes that as R,
               with the same zero/one values for its other components. */
            int depth_swizzle = descriptor->tic[0] == 0x60124a0eu &&
                                sampled_format == nvn_format(0x35).vk;
            if (semantic_shape && sampled_type == vk_tex_view_type(shader_texture) && num == 0) {
                if (color_feedback && sampled_width == command->color_target[color_feedback_index].width &&
                    sampled_height == command->color_target[color_feedback_index].height &&
                    sampled_layers == 1 && sampled_levels == 1 &&
                    sampled_format == nvn_format(command->color_target[color_feedback_index].format).vk) {
                    if (!color_feedback_view[color_feedback_index])
                        color_feedback_view[color_feedback_index] =
                            shadow_attachment_feedback(&command->color_target[color_feedback_index],
                                                       command->last_unmodeled_write, 0, descriptor->tic);
                    view = color_feedback_view[color_feedback_index];
                }
                else if (depth_feedback && sampled_width == depth_target->width &&
                    sampled_height == depth_target->height && sampled_layers == 1 && sampled_levels == 1 &&
                    sampled_format == nvn_format(depth_target->format).vk) {
                    if (!depth_feedback_view)
                        depth_feedback_view = shadow_attachment_feedback(depth_target,
                                                                         command->last_unmodeled_write, 1,
                                                                         descriptor->tic);
                    view = depth_feedback_view;
                }
                else if (!color_feedback && !depth_feedback && (identity_swizzle || depth_swizzle))
                    view = image_view_from_semantic(descriptor, effective_unknown);
            }
            if ((color_feedback || depth_feedback) && !view) return 0;
            semantic_sampled += view != VK_NULL_HANDLE;
            if (!view) {
                Surf* sampled = vk_tex_from_tic(descriptor->tic);
                if (!sampled) return 0;
                /* A distinct Maxwell target can still supply a sampled input while its NVN
                   producer is being modeled. Bound attachments require the snapshot above. */
                if (command->depth_target_bound && sampled->gva == depth_target->gpu_address) return 0;
                for (u32 i = 0; i < key->ncolor; i++)
                    if ((command->color_target_mask & (1u << i)) &&
                        sampled->gva == command->color_target[i].gpu_address) return 0;
                view = vk_tex_bind_view(sampled, vk_tex_view_type(shader_texture), descriptor->tic, num);
                if (view)
                    vk_surf_use(vk_cmd(), sampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            }
            VkSampler handle = vk_tex_bind_sampler(sampler->tsc, shader_texture, num);
            if (!view || !handle) return 0;
            textures[stage][tex_index] = (VkDescriptorImageInfo){handle, view,
                                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[nwrites++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                      .dstSet = sets[stage],
                                                      .dstBinding = SNX_VKB_TEX + tex_index,
                                                      .descriptorCount = 1,
                                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                      .pImageInfo = &textures[stage][tex_index]};
        }
        if (stage == 1)
            for (int k = 0; k < fragment->si.nimg; k++)
                writes[nwrites++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                          .dstSet = sets[stage],
                                                          .dstBinding = SNX_VKB_IMG + k,
                                                          .descriptorCount = 1,
                                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                          .pImageInfo = &storage_infos[k]};
    }
    if (setup_frame != vk_frame_number()) return 0;
    if (nwrites) vkUpdateDescriptorSets(g_vk.dev, nwrites, writes, 0, NULL);
    int full = 0;
    VkFmt color_format = nvn_format(color_target->format);
    VkFmt depth_format = command->depth_target_bound ? nvn_format(depth_target->format) : (VkFmt){0};
    NvnImage* colors[8] = {0};
    VkImageView color_views[8] = {0};
    for (u32 i = 0; i < key->ncolor; i++) {
        if (!(command->color_target_mask & (1u << i))) continue;
        const NvnTextureTarget* target = &command->color_target[i];
        VkFmt format = nvn_format(target->format);
        colors[i] = image_for(target, format.vk, VK_IMAGE_ASPECT_COLOR_BIT, &full);
        if (!colors[i] || !colors[i]->attachment) return 0;
        color_views[i] = shadow_attachment_view(colors[i], format.vk, target->view.base_level,
                                                target->view.first_layer);
        if (!color_views[i]) return 0;
        for (u32 j = 0; j < i; j++)
            if (colors[j] == colors[i] && command->color_target[j].view.base_level == target->view.base_level &&
                command->color_target[j].view.first_layer == target->view.first_layer) return 0;
    }
    NvnImage* color = colors[0];
    NvnImage* depth = command->depth_target_bound
                          ? image_for(depth_target, depth_format.vk, vk_aspect(&depth_format), &full) : NULL;
    if (command->depth_target_bound && (!depth || !depth->attachment)) return 0;
    u32 depth_mip = depth_target->view.base_level;
    if (depth && (command->depth_stencil_state[0] & 1) &&
        (!(depth->depth_layers[depth_mip] & (1ull << depth_layer)) ||
         depth->subresource_sequence[depth_mip][depth_layer] <= effective_unknown)) {
        return 0;
    }
    VkImageView depth_view = depth ? shadow_attachment_view(depth, depth_format.vk, depth_mip, depth_layer)
                              : VK_NULL_HANDLE;
    if (depth && !depth_view) return 0;
    vk_render_end();
    VkCommandBuffer cb = vk_cmd();
    NvnShadowStorage* used_storage[32] = {0};
    u8 used_writable[32] = {0};
    u32 used_count = 0;
    for (u32 stage = 0; stage < 2; stage++)
        for (int i = 0; i < storage.n[stage]; i++) {
            NvnShadowStorage* entry = shadow_buffers[stage][i];
            if (!entry) continue;
            u32 j = 0;
            while (j < used_count && used_storage[j] != entry) j++;
            if (j == used_count) used_storage[used_count++] = entry;
            used_writable[j] |= !!(storage_shaders[stage]->si.gbuf_wmask & (1u << i));
        }
    for (u32 i = 0; i < used_count; i++) {
        NvnShadowStorage* entry = used_storage[i];
        VkBufferMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                          .srcStageMask = entry->stage,
                                          .srcAccessMask = entry->access,
                                          .dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                          .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                           (used_writable[i] ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0),
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .buffer = entry->buffer,
                                          .offset = 0,
                                          .size = entry->size};
        VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                       .bufferMemoryBarrierCount = 1,
                                       .pBufferMemoryBarriers = &barrier};
        vkCmdPipelineBarrier2(cb, &dependency);
    }
    for (int k = 0; k < fragment->si.nimg; k++) {
        NvnImage* image = storage_images[k];
        vk_barrier(cb, image->image, VK_IMAGE_ASPECT_COLOR_BIT, image->stage, image->access, image->layout,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_IMAGE_LAYOUT_GENERAL);
        image->layout = VK_IMAGE_LAYOUT_GENERAL;
        image->stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        image->access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    for (u32 i = 0; i < key->ncolor; i++) {
        NvnImage* attachment = colors[i];
        if (!attachment) continue;
        vk_barrier(cb, attachment->image, VK_IMAGE_ASPECT_COLOR_BIT,
                   attachment->stage, attachment->access, attachment->layout,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        attachment->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment->stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        attachment->access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (depth)
        vk_barrier(cb, depth->image, vk_aspect(&depth_format), depth->stage, depth->access, depth->layout,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if (depth) {
        depth->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth->stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        depth->access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    VkRenderingAttachmentInfo color_attachments[8] = {0};
    for (u32 i = 0; i < key->ncolor; i++) {
        color_attachments[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!colors[i]) {
            color_attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            color_attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            continue;
        }
        color_attachments[i].imageView = color_views[i];
        color_attachments[i].imageLayout = colors[i]->layout;
        color_attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color_attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingAttachmentInfo depth_attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                  .imageView = depth_view,
                                                  .imageLayout = depth ? depth->layout : VK_IMAGE_LAYOUT_UNDEFINED,
                                                  .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    int color_written = (fragment->si.sph[18] & 15) && key->blend[0].mask;
    int prior_covered = (color->color_layers[color_mip] & (1ull << color_layer)) &&
                        color->subresource_sequence[color_mip][color_layer] > effective_unknown;
    static int sample_first = 1;
    static int force_capture = -1;
    if (force_capture < 0) {
        const char* opt = getenv("SNX_NVN_SHADOW_FORCE_CAPTURE");
        force_capture = opt && !strcmp(opt, "1");
    }
    int capture = sample_first && !color_mip && command->color_target_mask == 1 && color_written &&
                  (!prior_covered || force_capture) &&
                  (color_target->format == 0x25 || color_target->format == 0x38 ||
                   color_target->format == 0x29 || color_target->format == 0x3f);
    int capture_full = 0;
    if (capture) {
        color_attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachments[0].clearValue.color = (VkClearColorValue){{1.0f, 0.0f, 1.0f, 1.0f}};
        if (depth) {
            depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth_attachment.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
        }
    }
    u32 depth_width = depth && depth_mip < 16 ? depth_target->width >> depth_mip : 0;
    u32 depth_height = depth && depth_mip < 16 ? depth_target->height >> depth_mip : 0;
    if (depth && !depth_width) depth_width = 1;
    if (depth && !depth_height) depth_height = 1;
    u32 render_width = depth && depth_width < color_width ? depth_width : color_width;
    u32 render_height = depth && depth_height < color_height ? depth_height : color_height;
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                 .renderArea = {{0, 0}, {render_width, render_height}},
                                 .layerCount = 1,
                                 .colorAttachmentCount = key->ncolor,
                                 .pColorAttachments = color_attachments,
                                 .pDepthAttachment = depth ? &depth_attachment : NULL,
                                 .pStencilAttachment = depth && depth_format.stencil ? &depth_attachment : NULL};
    vkCmdBeginRendering(cb, &rendering);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk_playout, 0, 2, sets, 0, NULL);
    if (key->nbind)
        vkCmdBindVertexBuffers2(cb, 0, key->nbind, vertex_buffers, vertex_offsets, NULL, vertex_strides);
    /* NVN's positive-height top-left viewport is programmed as a negative-height Maxwell transform.
       Match that transform so face winding and screen-space texture coordinates agree with the guest. */
    VkViewport viewport = {vp_data[0], vp_data[1] + vp_data[3], vp_data[2], -vp_data[3], 0.0f, 1.0f};
    if (command->depth_range_known & 1)
        memcpy(&viewport.minDepth, command->depth_range[0], sizeof(float) * 2);
    vkCmdSetViewportWithCount(cb, 1, &viewport);
    VkRect2D scissor = {{(i32)sc_data[0], (i32)sc_data[1]}, {sc_data[2], sc_data[3]}};
    vkCmdSetScissorWithCount(cb, 1, &scissor);
    vkCmdSetCullMode(cb, (VkCullModeFlags)(command->polygon_state & 3));
    vkCmdSetFrontFace(cb, (command->polygon_state & 4) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                     : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, (indexed_triangles || triangles) ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                                   : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
    vkCmdSetRasterizerDiscardEnable(cb, VK_FALSE);
    vkCmdSetLineWidth(cb, 1.0f);
    vkCmdSetDepthBiasEnable(cb, fill_offset);
    if (fill_offset) {
        float factor, units, clamp;
        memcpy(&factor, &command->polygon_offset[0], sizeof factor);
        memcpy(&units, &command->polygon_offset[1], sizeof units);
        memcpy(&clamp, &command->polygon_offset[2], sizeof clamp);
        vkCmdSetDepthBias(cb, units, clamp, factor);
    } else {
        vkCmdSetDepthBias(cb, 0, 0, 0);
    }
    float blend[4] = {0};
    if (command->blend_color_known) memcpy(blend, command->blend_color, sizeof blend);
    vkCmdSetBlendConstants(cb, blend);
    u32 depth_state = command->depth_stencil_state[0];
    vkCmdSetDepthTestEnable(cb, depth && (depth_state & 1));
    vkCmdSetDepthWriteEnable(cb, depth && (depth_state & 2));
    vkCmdSetDepthCompareOp(cb, depth && (depth_state & 1)
                                   ? (VkCompareOp)(((depth_state >> 4) & 15) - 1)
                                   : VK_COMPARE_OP_ALWAYS);
    int depth_bounds = depth && command->depth_bounds_enabled;
    vkCmdSetDepthBoundsTestEnable(cb, depth_bounds);
    if (depth_bounds) {
        float bounds[2];
        memcpy(bounds, command->depth_bounds, sizeof bounds);
        vkCmdSetDepthBounds(cb, bounds[0], bounds[1]);
    } else {
        vkCmdSetDepthBounds(cb, 0.0f, 1.0f);
    }
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    float push[8] = {0, 1, 0, 0, 0, 0, 0, 0};
    vkCmdPushConstants(cb, g_vk_playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, push);
    if (indexed_triangles) {
        vkCmdBindIndexBuffer(cb, index_buffer, index_offset,
                             command->index_type == 0 ? VK_INDEX_TYPE_UINT8
                             : command->index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, command->count, command->instance_count, 0, command->base_vertex,
                         command->base_instance);
    } else {
        vkCmdDraw(cb, command->count, command->instance_count, command->first, command->base_instance);
    }
    if (vertex->si.gbuf_wmask || fragment->si.gbuf_wmask)
        for (u32 stage = 0; stage < 2; stage++)
            for (int i = 0; i < storage.n[stage]; i++)
                if (storage_shaders[stage]->si.gbuf_wmask & (1u << i))
                    LOG_LIMIT(16, "nvn-shadow-storage-writer: seq=%llu draw=%u vs=%llx fs=%llx stage=%u binding=%d addr=%llx size=%u target=%llx\n",
                              (unsigned long long)command->sequence, command->count,
                              (unsigned long long)vertex->hash, (unsigned long long)fragment->hash,
                              stage, i, (unsigned long long)storage.b[stage][i].addr,
                              storage.b[stage][i].size,
                              (unsigned long long)color_target->gpu_address);
    for (u32 i = 0; i < used_count; i++) {
        used_storage[i]->stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        used_storage[i]->access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  (used_writable[i] ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0);
    }
    vkCmdEndRendering(cb);
    if (capture) {
        u64 bytes = (u64)color_target->width * color_target->height * color_format.bpp;
        VkBuffer readback;
        u8* pixels = color->transfer_src && bytes <= (32ull << 20)
                         ? vk_readback_buffer(bytes, &readback) : NULL;
        if (pixels) {
            vk_barrier(cb, color->image, VK_IMAGE_ASPECT_COLOR_BIT,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            color->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            color->stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            color->access = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, color_layer, 1},
                                      .imageExtent = {color_target->width, color_target->height, 1}};
            vkCmdCopyImageToBuffer(cb, color->image, color->layout, readback, 1, &copy);
            vk_finish();
            vk_readback_ready();
            u64 changed = 0;
            const u8 magenta_rgba16f[8] = {0, 0x3c, 0, 0, 0, 0x3c, 0, 0x3c};
            const u32 magenta_packed = color_target->format == 0x3f ? 0x780003c0u : 0xffff00ffu;
            const void* sentinel = color_format.bpp == 8 ? (const void*)magenta_rgba16f : &magenta_packed;
            for (u64 i = 0; i < bytes; i += color_format.bpp)
                changed += memcmp(pixels + i, sentinel, color_format.bpp) != 0;
            capture_full = changed == bytes / color_format.bpp;
            u64 center = ((u64)(color_target->height / 2) * color_target->width + color_target->width / 2) *
                         color_format.bpp;
            LOG("nvn-shadow-readback: changed-pixels=%llu total=%llu corners=%02x%02x%02x%02x/%02x%02x%02x%02x center=%02x%02x%02x%02x\n",
                (unsigned long long)changed, (unsigned long long)(bytes / color_format.bpp), pixels[0], pixels[1], pixels[2],
                pixels[3], pixels[bytes - 4], pixels[bytes - 3], pixels[bytes - 2], pixels[bytes - 1],
                pixels[center], pixels[center + 1], pixels[center + 2], pixels[center + 3]);
        }
        sample_first = 0;
    }
    for (u32 i = 0; i < key->ncolor; i++)
        if (colors[i]) colors[i]->last_batch = batch_serial;
    if (depth) depth->last_batch = batch_serial;
    for (int k = 0; k < fragment->si.nimg; k++) {
        storage_images[k]->color_layers[0] &= ~1ull;
        storage_images[k]->last_batch = batch_serial;
    }
    /* The magenta first-draw probe certifies full coverage only when no pixel retains that initial color.
       Other draws preserve provenance only after modeled full-subresource initialization. */
    int preserved = prior_covered;
    int any_color_written = 0;
    for (u32 i = 0; i < key->ncolor; i++) {
        NvnImage* attachment = colors[i];
        if (!attachment) continue;
        const NvnTextureTarget* target = &command->color_target[i];
        u32 mip = target->view.base_level;
        u32 layer = target->view.first_layer;
        int written = ((fragment->si.sph[18] >> (i * 4)) & 15) && key->blend[i].mask;
        if (!written) continue;
        any_color_written = 1;
        int covered = (attachment->color_layers[mip] & (1ull << layer)) &&
                      attachment->subresource_sequence[mip][layer] > effective_unknown;
        if (i == 0) covered = capture ? capture_full : prior_covered;
        if (covered) {
            attachment->color_layers[mip] |= 1ull << layer;
            attachment->subresource_sequence[mip][layer] = command->sequence;
            if (!attachment->guest_host) shadow_record_backing(attachment);
        } else
            attachment->color_layers[mip] &= ~(1ull << layer);
        if (i == 0) preserved = covered;
        attachment->shadow_draw_stamp = ++shadow_draw_stamp;
    }
    mark_modeled_unknown(command->sequence + 1, command->last_unmodeled_write);
    static u64 issued;
    static u64 preserved_count, semantic_sample_count, embedded_bank1_count, storage_draw_count, color_write_count;
    static u64 ssbo_count, writable_ssbo_count;
    issued++;
    preserved_count += preserved;
    semantic_sample_count += semantic_sampled;
    embedded_bank1_count += !!(cbuf_mask & 2);
    storage_draw_count += !!fragment->si.nimg;
    ssbo_count += !!(vertex->si.ngbuf || fragment->si.ngbuf);
    writable_ssbo_count += !!(vertex->si.gbuf_wmask || fragment->si.gbuf_wmask);
    color_write_count += any_color_written;
    if (issued <= 8 || !(issued % 100))
        LOG("nvn-shadow-draw: issued=%llu preserved=%llu semantic-samples=%llu embedded-bank1=%llu storage=%llu ssbo=%llu writable-ssbo=%llu color-writes=%llu target=%llx size=%ux%u\n",
            (unsigned long long)issued, (unsigned long long)preserved_count,
            (unsigned long long)semantic_sample_count, (unsigned long long)embedded_bank1_count,
            (unsigned long long)storage_draw_count, (unsigned long long)ssbo_count,
            (unsigned long long)writable_ssbo_count,
            (unsigned long long)color_write_count,
            (unsigned long long)color_target->gpu_address,
            color_target->width, color_target->height);
    return 1;
}

/* A failed direct draw may still have a bounded storage write set. Its NVN storage binds carry
   the address and size, while a validated SDK macro packet places that binding in CBUF 0.
   Require contiguous guest backing for every covered image and writable buffer: a missing
   translation must leave the global unknown marker intact. */
static int scope_storage_writes(const NvnDrawCommand* command, const VkSh* vertex, const VkSh* fragment) {
    typedef struct { u32 size; const u8* host; } StorageWrite;
    StorageWrite writes[32];
    u8 invalidate[NVN_IMAGE_SLOTS] = {0};
    u32 count = 0;
    const VkSh* shaders[2] = {vertex, fragment};
    for (u32 s = 0; s < 2; s++) {
        const ShaderInfo* si = &shaders[s]->si;
        if (!si->gbuf_wmask) continue;
        if (si->ngbuf < 0 || si->ngbuf > 16 || (si->gbuf_wmask >> si->ngbuf)) return 0;
        for (u32 i = 0; i < (u32)si->ngbuf; i++) {
            if (!(si->gbuf_wmask & (1u << i))) continue;
            u32 off = si->gbuf_cbuf_off[i];
            const NvnStorageBinding* selected = NULL;
            for (u32 slot = 0; slot < NVN_DRAW_STORAGE_SLOTS; slot++) {
                if (!(command->storage_known[s] & (1u << slot))) continue;
                const NvnStorageBinding* binding = &command->storage[s][slot];
                u32 candidate = 0;
                if (!gpu_unique_cb_offset_macro(0xe20, binding->cbuf_word, s ? 4 : 0, &candidate)) return 0;
                if (candidate != off) continue;
                if (selected) return 0;
                selected = binding;
            }
            if (!selected) return 0;
            u64 address = selected->range.address;
            if (selected->range.size > (16u << 20)) return 0;
            u32 size = (u32)selected->range.size;
            if (!size) size = 16;
            size = (size + 3) & ~3u;
            if (!address || address > UINT64_MAX - size) return 0;
            const u8* host = gmem_ptr_untracked(address, size);
            if (!host) return 0;
            writes[count++] = (StorageWrite){size, host};
        }
    }
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        const NvnImage* image = &images[i];
        if (!image->image) continue;
        int covered = 0;
        for (u32 m = 0; m < 16; m++) covered |= !!(image->color_layers[m] | image->depth_layers[m]);
        if (!covered) continue;
        if (!image->storage_size || image->address > UINT64_MAX - image->storage_size) return 0;
        const u8* image_host = gmem_ptr_untracked(image->address, image->storage_size);
        if (!image_host) return 0;
        u64 a = (u64)(uintptr_t)image_host;
        for (u32 j = 0; j < count; j++) {
            u64 b = (u64)(uintptr_t)writes[j].host;
            if (a > UINT64_MAX - image->storage_size || b > UINT64_MAX - writes[j].size) return 0;
            if (a < b + writes[j].size && b < a + image->storage_size) invalidate[i] = 1;
        }
    }
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        NvnImage* image = &images[i];
        if (!invalidate[i]) continue;
        memset(image->color_layers, 0, sizeof image->color_layers);
        memset(image->depth_layers, 0, sizeof image->depth_layers);
        image->guest_host = NULL;
    }
    return 1;
}

/* A decoded guest-memory write can retire only the semantic resources it aliases. A missing physical
   translation keeps the global marker intact, since a GPU address alone cannot prove non-aliasing. */
static int reject_memory_write(const NvnMemoryWriteCommand* command) {
    /* A preceding draw may have bridged sequence+1, which is also this write's sequence. The failed
       write must revoke that bridge rather than inheriting its proof. */
    if (command && modeled_unknown_marker == command->sequence)
        modeled_unknown_marker = modeled_unknown_prior = 0;
    return 0;
}

int vk_nvn_memory_write(const NvnMemoryWriteCommand* command) {
    if (!command || !command->address || !command->size || command->size > (16u << 20) ||
        command->address > UINT64_MAX - command->size)
        return reject_memory_write(command);
    const u8* host = gmem_ptr_untracked(command->address, command->size);
    if (!host) return reject_memory_write(command);
    u8 invalidate_image[NVN_IMAGE_SLOTS] = {0};
    u8 invalidate_storage[NVN_SHADOW_STORAGE_SLOTS] = {0};
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        NvnImage* image = &images[i];
        if (!image->image) continue;
        int covered = 0;
        for (u32 level = 0; level < 16; level++) covered |= !!(image->color_layers[level] | image->depth_layers[level]);
        if (!covered) continue;
        if (!image->storage_size || image->address > UINT64_MAX - image->storage_size)
            return reject_memory_write(command);
        const u8* image_host = gmem_ptr_untracked(image->address, image->storage_size);
        if (!image_host) return reject_memory_write(command);
        invalidate_image[i] = shadow_host_overlap(host, command->size, image_host, image->storage_size);
    }
    for (u32 i = 0; i < NVN_SHADOW_STORAGE_SLOTS; i++) {
        NvnShadowStorage* entry = &shadow_storage[i];
        if (!entry->buffer) continue;
        if (!entry->guest_host || !entry->size) return reject_memory_write(command);
        invalidate_storage[i] = shadow_host_overlap(host, command->size, entry->guest_host, entry->size);
    }
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        if (!invalidate_image[i]) continue;
        NvnImage* image = &images[i];
        memset(image->color_layers, 0, sizeof image->color_layers);
        memset(image->depth_layers, 0, sizeof image->depth_layers);
        image->guest_host = NULL;
    }
    for (u32 i = 0; i < NVN_SHADOW_STORAGE_SLOTS; i++)
        if (invalidate_storage[i]) shadow_storage[i].invalid = 1;
    mark_modeled_unknown(command->sequence, command->prior_unmodeled_write);
    static u64 scoped_writes, scoped_cbuf_writes;
    scoped_writes++;
    scoped_cbuf_writes += command->kind == NVN_GAP_WRITE_CBUF;
    if (scoped_writes <= 8 || !(scoped_writes % 1000))
        LOG("nvn-shadow-memory-write: scoped=%llu cbuf=%llu addr=%llx size=%u marker=%llu\n",
            (unsigned long long)scoped_writes, (unsigned long long)scoped_cbuf_writes,
            (unsigned long long)command->address, command->size, (unsigned long long)command->sequence);
    return 1;
}

/* A guest draw can be ineligible for HLE while still having a fully known write set. Preserve
   unrelated shadow images across that draw, but discard every target it could have modified. */
static void scope_unmodeled_draw(const NvnDrawCommand* command, const VkSh* vertex, const VkSh* fragment) {
    const char* shadow = getenv("SNX_NVN_SHADOW_DRAW");
    /* Read-only SSBOs cannot add writes beyond the captured attachments. */
    if (!shadow || strcmp(shadow, "1") || !vertex || !fragment || !command->target_complete ||
        (!command->color_target_mask && !command->depth_target_bound) ||
        vertex->si.nimg || fragment->si.nimg)
        return;
    for (u32 i = 0; i < 8; i++)
        if ((command->color_target_mask & (1u << i)) && !shadow_range_known(&command->color_target[i])) return;
    if (command->depth_target_bound && !shadow_range_known(&command->depth_target)) return;
    int storage_writes = !!(vertex->si.gbuf_wmask || fragment->si.gbuf_wmask);
    if (storage_writes && !scope_storage_writes(command, vertex, fragment)) return;
    for (u32 i = 0; i < 8; i++)
        if (command->color_target_mask & (1u << i)) shadow_invalidate_backing(&command->color_target[i]);
    if (command->depth_target_bound) shadow_invalidate_backing(&command->depth_target);
    mark_modeled_unknown(command->sequence + 1, command->last_unmodeled_write);
    static u64 scoped_draws, scoped_storage_draws;
    scoped_draws++;
    scoped_storage_draws += storage_writes;
    if (scoped_draws == 1 || !(scoped_draws % 100))
        LOG("nvn-shadow-scoped-draw: count=%llu storage=%llu target=%llx colors=%x depth=%d\n",
            (unsigned long long)scoped_draws, (unsigned long long)scoped_storage_draws,
            (unsigned long long)command->color_target[0].gpu_address,
            command->color_target_mask, command->depth_target_bound);
}

/* Compare the first completed shadow target with its Maxwell surface after the guest submission. This is a
   diagnostic of the corresponding intermediate image, which need not be the VI-presented image. */
void vk_nvn_shadow_compare(void) {
    static int compared;
    static u64 eligible_count;
    if (compared || !shadow_draw_stamp) return;
    const char* size_filter = getenv("SNX_NVN_SHADOW_COMPARE_SIZE");
    const char* format_filter = getenv("SNX_NVN_SHADOW_COMPARE_FORMAT");
    unsigned filter_width = 0, filter_height = 0;
    unsigned long filter_format = format_filter ? strtoul(format_filter, NULL, 0) : 0;
    if (size_filter && (sscanf(size_filter, "%ux%u", &filter_width, &filter_height) != 2 ||
                        !filter_width || !filter_height))
        return;
    NvnImage* shadow = NULL;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++)
        if (images[i].shadow_draw_stamp &&
            (!size_filter || (images[i].width == filter_width && images[i].height == filter_height)) &&
            (!format_filter || images[i].format == filter_format) &&
            (!shadow || images[i].shadow_draw_stamp > shadow->shadow_draw_stamp))
            shadow = &images[i];
    if (!shadow || !shadow->transfer_src || shadow->levels != 1 || shadow->layers != 1 ||
        !shadow->width || !shadow->height) {
        LOG_LIMIT(8, "nvn-shadow-compare-skip: shadow=%d transfer=%d levels=%u layers=%u size=%ux%u\n",
                  !!shadow, shadow ? shadow->transfer_src : 0, shadow ? shadow->levels : 0,
                  shadow ? shadow->layers : 0, shadow ? shadow->width : 0, shadow ? shadow->height : 0);
        return;
    }
    VkFmt format = nvn_format(shadow->format);
    if (!format.vk || !format.bpp || format.depth || format.bw > 1 || format.bh > 1 ||
        (u64)shadow->width * shadow->height > (16ull << 20) / format.bpp) {
        LOG_LIMIT(8, "nvn-shadow-compare-skip: format=%x vk=%u bpp=%u block=%ux%u depth=%d\n",
                  shadow->format, format.vk, format.bpp, format.bw, format.bh, format.depth);
        return;
    }
    Surf* emulated = vk_rt_find(shadow->address);
    int rgba8_transfer_pair = emulated &&
        ((format.vk == VK_FORMAT_R8G8B8A8_UNORM && emulated->f.vk == VK_FORMAT_R8G8B8A8_SRGB) ||
         (format.vk == VK_FORMAT_R8G8B8A8_SRGB && emulated->f.vk == VK_FORMAT_R8G8B8A8_UNORM));
    /* sRGB and UNORM RGBA8 views have identical stored bytes. Color conversion happens on access,
       so the diagnostic can compare their raw readbacks without changing either image. */
    if (!emulated || (emulated->f.vk != format.vk && !rgba8_transfer_pair) ||
        emulated->f.bpp != format.bpp ||
        emulated->w != shadow->width ||
        emulated->h != shadow->height || emulated->sw != emulated->w || emulated->sh != emulated->h ||
        emulated->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        LOG_LIMIT(8, "nvn-shadow-compare-skip: surface target=%llx found=%d format=%u/%u bpp=%u/%u size=%ux%u/%ux%u storage=%ux%u layout=%u\n",
                  (unsigned long long)shadow->address, !!emulated, format.vk,
                  emulated ? emulated->f.vk : 0, format.bpp, emulated ? emulated->f.bpp : 0,
                  shadow->width, shadow->height, emulated ? emulated->w : 0, emulated ? emulated->h : 0,
                  emulated ? emulated->sw : 0, emulated ? emulated->sh : 0,
                  emulated ? emulated->layout : 0);
        return;
    }
    eligible_count++;
    const char* present_filter = getenv("SNX_NVN_SHADOW_COMPARE_PRESENT");
    if (present_filter) {
        char* end = NULL;
        unsigned long requested = strtoul(present_filter, &end, 0);
        if (!requested || !end || *end || eligible_count != requested) return;
    }
    u64 bytes = (u64)shadow->width * shadow->height * format.bpp;
    VkBuffer readback;
    u8* pixels = vk_readback_buffer(bytes * 2, &readback);
    if (!pixels) return;
    VkCommandBuffer cb = vk_cmd();
    vk_barrier(cb, shadow->image, VK_IMAGE_ASPECT_COLOR_BIT, shadow->stage, shadow->access, shadow->layout,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    shadow->stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    shadow->access = VK_ACCESS_2_TRANSFER_READ_BIT;
    shadow->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vk_surf_use(cb, emulated, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {shadow->width, shadow->height, 1}};
    vkCmdCopyImageToBuffer(cb, shadow->image, shadow->layout, readback, 1, &copy);
    copy.bufferOffset = bytes;
    vkCmdCopyImageToBuffer(cb, emulated->img, emulated->layout, readback, 1, &copy);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                   .memoryBarrierCount = 1,
                                   .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cb, &dependency);
    vk_finish();
    vk_readback_ready();
    u64 different = 0, different_bytes = 0, first_different = ~0ull;
    u64 nonblack_shadow = 0, nonblack_emulated = 0;
    const u8 black_rgba8[4] = {0, 0, 0, 255};
    const u8 black_rgba16f[8] = {0, 0, 0, 0, 0, 0, 0, 0x3c};
    const u8* black = shadow->format == 0x25 ? black_rgba8
                    : shadow->format == 0x29 ? black_rgba16f : NULL;
    u64 components[4] = {0};
    u32 min_x = shadow->width, min_y = shadow->height, max_x = 0, max_y = 0;
    for (u64 i = 0; i < bytes; i += format.bpp) {
        if (black) {
            nonblack_shadow += memcmp(pixels + i, black, format.bpp) != 0;
            nonblack_emulated += memcmp(pixels + bytes + i, black, format.bpp) != 0;
        }
        if (memcmp(pixels + i, pixels + bytes + i, format.bpp)) {
            different++;
            if (first_different == ~0ull) first_different = i;
            u64 pixel = i / format.bpp;
            u32 x = (u32)(pixel % shadow->width), y = (u32)(pixel / shadow->width);
            if (x < min_x) min_x = x;
            if (y < min_y) min_y = y;
            if (x > max_x) max_x = x;
            if (y > max_y) max_y = y;
        }
        for (u32 b = 0; b < format.bpp; b++)
            different_bytes += pixels[i + b] != pixels[bytes + i + b];
        if (format.bpp == 8)
            for (u32 component = 0; component < 4; component++)
                components[component] += memcmp(pixels + i + component * 2,
                                                pixels + bytes + i + component * 2, 2) != 0;
    }
    u64 center = ((u64)(shadow->height / 2) * shadow->width + shadow->width / 2) * format.bpp;
    LOG("nvn-shadow-compare: eligible=%llu target=%llx format=%x bpp=%u different=%llu/%llu bytes=%llu/%llu emulated-wgen=%u center=%02x%02x%02x%02x/%02x%02x%02x%02x\n",
        (unsigned long long)eligible_count, (unsigned long long)shadow->address, shadow->format, format.bpp,
        (unsigned long long)different, (unsigned long long)(bytes / format.bpp),
        (unsigned long long)different_bytes, (unsigned long long)bytes, emulated->wgen,
        pixels[center], pixels[center + 1], pixels[center + 2], pixels[center + 3],
        pixels[bytes + center], pixels[bytes + center + 1], pixels[bytes + center + 2], pixels[bytes + center + 3]);
    if (black)
        LOG("nvn-shadow-coverage: target=%llx nonblack=%llu/%llu emulated=%llu/%llu stamp=%llu\n",
            (unsigned long long)shadow->address, (unsigned long long)nonblack_shadow,
            (unsigned long long)(bytes / format.bpp), (unsigned long long)nonblack_emulated,
            (unsigned long long)(bytes / format.bpp), (unsigned long long)shadow->shadow_draw_stamp);
    if (different) {
        u64 first_shadow = 0, first_emulated = 0;
        memcpy(&first_shadow, pixels + first_different, format.bpp < 8 ? format.bpp : 8);
        memcpy(&first_emulated, pixels + bytes + first_different, format.bpp < 8 ? format.bpp : 8);
        LOG("nvn-shadow-difference: bbox=%u,%u-%u,%u first=%llu,%llu values=%016llx/%016llx components=%llu,%llu,%llu,%llu\n",
            min_x, min_y, max_x, max_y,
            (unsigned long long)((first_different / format.bpp) % shadow->width),
            (unsigned long long)((first_different / format.bpp) / shadow->width),
            (unsigned long long)first_shadow, (unsigned long long)first_emulated,
            (unsigned long long)components[0], (unsigned long long)components[1],
            (unsigned long long)components[2], (unsigned long long)components[3]);
    }
    compared = 1;
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
    u64 embedded_bank1[2] = {0};
    int shader_ready = command->materialize_shaders &&
                       materialize_draw_shaders(command, &vertex_shader, &fragment_shader, embedded_bank1);
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
                if (!surf) {
                    failure = NVN_DRAW_TEXTURE_IMAGE_MISSING;
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
    PipeKey draw_key = {0};
    VkPipeline draw_pipeline = VK_NULL_HANDLE;
    if (command->prepare_pipeline) {
        if ((resource_status & NVN_DRAW_SHADER_MODULE_READY) &&
            (resource_status & NVN_DRAW_VERTEX_FORMAT_READY) &&
            (resource_status & NVN_DRAW_ATTACHMENT_READY))
            draw_pipeline = prepare_draw_pipeline(command, vertex_shader, fragment_shader, &draw_key);
        resource_status |= draw_pipeline ? NVN_DRAW_PIPELINE_READY : NVN_DRAW_PIPELINE_FAILED;
        /* A known draw still writes its targets when its direct pipeline cannot be built. Invalidate
           those shadow attachments before returning from any of the surface checks below. */
        if (!draw_pipeline) scope_unmodeled_draw(command, vertex_shader, fragment_shader);
    }
    /* Shadow rendering owns a separate NVN image. Maxwell's surface cache can expose another mip, layer, or
       dimensions at the same address; those comparisons are diagnostics, not prerequisites for an NVN draw. */
    if (draw_pipeline &&
        !shadow_draw_simple(command, vertex_shader, fragment_shader, draw_pipeline, &draw_key, embedded_bank1))
        scope_unmodeled_draw(command, vertex_shader, fragment_shader);
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
