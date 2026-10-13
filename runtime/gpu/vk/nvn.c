/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN-owned probe images. They remain separate from guest Maxwell surfaces until whole-list replacement and
   guest-memory coherence are implemented. */
#include "gpu/vk/vk.h"
#include "gpu/gpu.h"
#include "gpu/shader/shader.h"
#include "hle/nvdrv.h"
#include "platform/log.h"
#include <stdlib.h>
#include <string.h>

#define NVN_IMAGE_SLOTS  128
#define NVN_IMAGE_BUDGET (256ull << 20)
typedef struct {
    VkImageView view;
    u16 mip, layer;
} NvnAttachmentView;
typedef struct {
    u64 texture, pool, address, texture_generation, pool_generation;
    u64 subresource_sequence[16][64];
    u32 width, height, levels, layers, target, format;
    u64 bytes;
    u64 last_batch;
    VkImage image;
    VkImageView sampled_view, base_view;
    NvnAttachmentView attachments[16];
    VmaAllocation allocation;
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    u64 color_layers[16], depth_layers[16];
    int sampled;
    int attachment;
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
        case 0x0d: return (VkFmt){.vk = VK_FORMAT_R8G8_UNORM, .bpp = 2};          /* RG8 */
        case 0x11: return (VkFmt){.vk = VK_FORMAT_R16G16_SFLOAT, .bpp = 4};       /* RG16F */
        case 0x12: return (VkFmt){.vk = VK_FORMAT_R16G16_UNORM, .bpp = 4};        /* RG16 */
        case 0x25: return vk_rt_format(0xd5);                                     /* RGBA8 */
        case 0x29: return (VkFmt){.vk = VK_FORMAT_R16G16B16A16_SFLOAT, .bpp = 8}; /* RGBA16F */
        case 0x38: return (VkFmt){.vk = VK_FORMAT_R8G8B8A8_SRGB, .bpp = 4};       /* RGBA8 SRGB */
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
            image->target == target->target && image->format == target->format)
            return image;
    }
    VkFmt guest_format = nvn_format(target->format);
    const VkPhysicalDeviceLimits* limits = &g_vk.props.limits;
    if (!guest_format.bpp || !target->levels || !target->depth || target->depth > limits->maxImageArrayLayers ||
        target->width > limits->maxImageDimension2D || target->height > limits->maxImageDimension2D)
        return NULL;
    if (target->target == 8 &&
        (target->width != target->height || target->width > limits->maxImageDimensionCube || target->depth != 6))
        return NULL;
    u32 max_dimension = target->width > target->height ? target->width : target->height;
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
                 guest_format.bpp * target->depth;
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
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, format, &properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) return NULL;
    int sampled = !!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    VkFormatFeatureFlags attachment_feature = aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                                                  ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                  : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    int attachment = !!(properties.optimalTilingFeatures & attachment_feature);
    VkImageUsageFlags attachment_usage =
        attachment ? (aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                         : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
                   : 0;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .flags = target->target == 8 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                              .format = format,
                              .extent = {target->width, target->height, 1},
                              .mipLevels = target->levels,
                              .arrayLayers = target->depth,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
                                       attachment_usage};
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
    empty->target = target->target;
    empty->format = target->format;
    empty->bytes = bytes;
    image_bytes += bytes;
    empty->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    empty->stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    empty->sampled = sampled;
    empty->attachment = attachment;
    if (sampled) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = image,
                                      .viewType = target->target == 8   ? VK_IMAGE_VIEW_TYPE_CUBE
                                                  : target->target == 4 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                        : VK_IMAGE_VIEW_TYPE_2D,
                                      .format = format,
                                      .subresourceRange = {aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                                                               ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                               : VK_IMAGE_ASPECT_COLOR_BIT,
                                                           0, target->levels, 0, target->depth}};
        if (vkCreateImageView(g_vk.dev, &view, NULL, &empty->sampled_view) != VK_SUCCESS) empty->sampled = 0;
        if (empty->sampled && target->levels > 1) {
            view.subresourceRange.levelCount = 1;
            vkCreateImageView(g_vk.dev, &view, NULL, &empty->base_view);
        }
    }
    return empty;
}

/* Materialize the exact mip and layer a future host draw would attach. No pixels are marked written until that draw
   actually executes on the host path. */
static int prepare_attachment(const NvnTextureTarget* target, int depth) {
    u32 mip = target->view.base_level, layer = target->view.first_layer;
    if ((target->target != 1 && target->target != 4 && target->target != 8) || !target->width || !target->height ||
        !target->depth || (target->target == 1 && target->depth != 1) || !target->levels || target->samples > 1 ||
        mip >= target->levels || layer >= target->depth || target->view.level_count > 1 ||
        target->view.layer_count > 1 || (target->view.flags & (8 | 16)))
        return 0;
    VkFmt format = nvn_format(target->format);
    if (!format.vk || !!format.depth != depth || format.bw > 1 || format.bh > 1 || format.conv ||
        (target->format == 0x25 && format.vk != VK_FORMAT_R8G8B8A8_UNORM) ||
        (target->format == 0x3f && format.vk != VK_FORMAT_B10G11R11_UFLOAT_PACK32))
        return 0;
    int full = 0;
    NvnImage* image = image_for(target, format.vk, vk_aspect(&format), &full);
    if (!image || !image->attachment) return 0;
    for (u32 i = 0; i < 16; i++)
        if (image->attachments[i].view && image->attachments[i].mip == mip && image->attachments[i].layer == layer) {
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
        image->last_batch = batch_serial;
        return 1;
    }
    return 0;
}

int vk_nvn_clear(const NvnClearCommand* command) {
    const NvnTextureTarget* target = &command->target;
    u32 mip = target->view.base_level;
    u32 layer = target->view.first_layer;
    if ((target->target != 1 && target->target != 4 && target->target != 8) || !target->width || !target->height ||
        !target->depth || (target->target == 1 && target->depth != 1) || !target->levels || target->samples > 1 ||
        mip >= target->levels || mip >= 16 || layer >= target->depth || target->view.level_count > 1 ||
        target->view.layer_count > 1 || (target->view.flags & (8 | 16))) {
        LOG_LIMIT(12, "nvn-clear-shape: target=%u %ux%ux%u levels=%u samples=%u view=%x/%u/%u/%u/%u\n", target->target,
                  target->width, target->height, target->depth, target->levels, target->samples, target->view.flags,
                  mip, layer, target->view.level_count, target->view.layer_count);
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
            image->address != address || image->width != width || image->height != height ||
            (levels != image->levels && (levels != 1 || !image->base_view)))
            continue;
        VkFmt source = nvn_format(image->format);
        if (source.vk != format) continue;
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
        return levels == 1 && image->levels > 1 ? image->base_view : image->sampled_view;
    }
    return VK_NULL_HANDLE;
}

/* Only the NVN blob wrapper and Maxwell SPH are needed to select a stage. An unrecognized
   reference must stay unresolved; SetShadersExt does not always put code in its first word. */
static int shader_code_stage(u64 address, u32* type) {
    if (!address || address > UINT64_MAX - 0x34) return -1;
    u32 header[2], sph;
    gmem_read_code(address, header, sizeof header);
    if (header[0] != 0x12345678 || header[1] < 0x80 || header[1] > 0x10030) return -1;
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
        case 0x0d: return VK_FORMAT_R8G8_UNORM;
        case 0x16: return VK_FORMAT_R32G32_SFLOAT;
        case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
        case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
        case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 0x2a: return VK_FORMAT_R16G16B16A16_UNORM;
        case 0x2e: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
        default: return VK_FORMAT_UNDEFINED;
    }
}

static int prepare_vertex_formats(const NvnDrawCommand* command) {
    for (u32 i = 0; i < command->vertex_attrib_count; i++) {
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
                                     stage ? 0 : attr_sint, stage ? rt_int : 0, stage ? rt_sint : 0, 0);
        memcpy(g_vk_cb[row], saved_cb, sizeof saved_cb);
        memcpy(g_vk_cbsize[row], saved_size, sizeof saved_size);
        if (!shader || !shader->mod || shader->failed || shader->si.unsupported) {
            LOG_LIMIT(8, "nvn-shader-module: compile failure stage=%u code=%llx failed=%d unsupported=%d\n", stage,
                      (unsigned long long)(stage ? fragment : (vertex_b ? vertex_b : vertex_a)),
                      shader ? shader->failed : -1, shader ? shader->si.unsupported : -1);
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
    if (!vertex || !fragment_shader || !command->channel_mask_known || !command->color_state_known ||
        !command->polygon_state_known || !command->depth_stencil_state_known || !command->multisample_state_known)
        return 0;
    u32 multisample = command->multisample_state[0];
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
    for (u32 location = 0; location < 32; location++) {
        if (!(vertex->si.in_mask & (1u << location))) continue;
        if (location >= command->vertex_attrib_count || key.nattr >= limits->maxVertexInputAttributes ||
            key.nbind >= limits->maxVertexInputBindings) {
            LOG_LIMIT(8, "nvn-pipeline: missing attribute location=%u count=%u bind=%u\n", location,
                      command->vertex_attrib_count, key.nbind);
            return 0;
        }
        const NvnVertexAttrib* attrib = &command->vertex_attrib[location];
        if (!attrib->enabled || attrib->stream >= command->vertex_stream_count ||
            !(command->vertex_bound & (1u << attrib->stream))) {
            LOG_LIMIT(8, "nvn-pipeline: missing stream location=%u stream=%u count=%u bound=%x\n", location,
                      attrib->stream, command->vertex_stream_count, command->vertex_bound);
            return 0;
        }
        VkFormat format = nvn_vertex_format(attrib->format);
        if (!format || command->vertex_stream[attrib->stream].stride > limits->maxVertexInputBindingStride) {
            LOG_LIMIT(8, "nvn-pipeline: invalid format or stride location=%u format=%x stride=%u\n", location,
                      attrib->format, command->vertex_stream[attrib->stream].stride);
            return 0;
        }
        u32 divisor = command->vertex_stream[attrib->stream].divisor;
        if (divisor > 1 && !g_vk.has_divisor) {
            LOG_LIMIT(8, "nvn-pipeline: unsupported divisor=%u\n", divisor);
            return 0;
        }
        u32 binding = key.nbind++;
        key.bind[binding].divisor = divisor;
        key.attr[key.nattr].loc = location;
        key.attr[key.nattr].binding = binding;
        key.attr[key.nattr].format = format;
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
    if ((depth_state & 3) && !key.depth) {
        LOG_LIMIT(8, "nvn-pipeline: depth enabled without depth target state=%08x\n", depth_state);
        return 0;
    }
    if ((depth_state & 1) && (!((depth_state >> 4) & 0xf) || ((depth_state >> 4) & 0xf) > 8)) {
        LOG_LIMIT(8, "nvn-pipeline: unsupported depth compare state=%08x\n", depth_state);
        return 0;
    }
    if (depth_state & 4) {
        if (!key.stencil) {
            LOG_LIMIT(8, "nvn-pipeline: stencil enabled without stencil target state=%08x\n", depth_state);
            return 0;
        }
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
    int sampler_status = 0, has_sampler = 0;
    for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES && !sampler_status; stage++)
        for (u32 i = 0; i < NVN_DRAW_TEXTURE_SLOTS; i++) {
            if (!(command->texture_bound[stage] & (1u << i))) continue;
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
    for (u32 stage = 0; stage < NVN_DRAW_SHADER_STAGES && !texture_status; stage++)
        for (u32 i = 0; i < NVN_DRAW_TEXTURE_SLOTS; i++) {
            if (!(command->texture_bound[stage] & (1u << i))) continue;
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
                if (!(command->texture_bound[stage] & (1u << i))) continue;
                VkImageView view =
                    image_view_from_semantic(&command->texture_descriptor[stage][i], command->last_unmodeled_write);
                if (view) {
                    from_clear = 1;
                    continue;
                }
                Surf* surf = vk_tex_from_tic(command->texture_descriptor[stage][i].tic);
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
    VkSh* vertex_shader = NULL;
    VkSh* fragment_shader = NULL;
    if (command->materialize_shaders)
        resource_status |=
            prepare_vertex_formats(command) ? NVN_DRAW_VERTEX_FORMAT_READY : NVN_DRAW_VERTEX_FORMAT_FAILED;
    if (command->materialize_shaders)
        resource_status |= materialize_draw_shaders(command, &vertex_shader, &fragment_shader)
                               ? NVN_DRAW_SHADER_MODULE_READY
                               : NVN_DRAW_SHADER_MODULE_FAILED;
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
