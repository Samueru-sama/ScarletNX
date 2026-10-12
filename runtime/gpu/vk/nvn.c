/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN-owned probe images. They remain separate from guest Maxwell surfaces until whole-list replacement and
   guest-memory coherence are implemented. */
#include "gpu/vk/vk.h"
#include "gpu/gpu.h"
#include "hle/nvdrv.h"
#include "platform/log.h"
#include <stdlib.h>
#include <string.h>

#define NVN_IMAGE_SLOTS  128
#define NVN_IMAGE_BUDGET (256ull << 20)
typedef struct {
    u64 texture, pool, address, texture_generation, pool_generation;
    u64 mip_sequence[16];
    u32 width, height, levels, layers, target, format;
    u64 bytes;
    u64 last_batch;
    VkImage image;
    VkImageView sampled_view, base_view;
    VmaAllocation allocation;
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    VkImageAspectFlags mip_aspects[16];
    int sampled;
} NvnImage;
static NvnImage images[NVN_IMAGE_SLOTS];
static u64 image_bytes;
static u64 batch_serial;

void vk_nvn_batch_begin(void) {
    batch_serial++;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) memset(images[i].mip_aspects, 0, sizeof images[i].mip_aspects);
}

static NvnImage* evict_old_image(void) {
    NvnImage* oldest = NULL;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        NvnImage* image = &images[i];
        if (!image->image || image->last_batch == batch_serial) continue;
        if (!oldest || image->last_batch < oldest->last_batch) oldest = image;
    }
    if (!oldest) return NULL;
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
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .flags = target->target == 8 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                              .format = format,
                              .extent = {target->width, target->height, 1},
                              .mipLevels = target->levels,
                              .arrayLayers = target->depth,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : 0)};
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
    if (target->target == 1) {
        image->mip_sequence[mip] = command->sequence;
        image->mip_aspects[mip] = range.aspectMask;
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
    image->mip_sequence[mip] = command->sequence;
    image->mip_aspects[mip] = VK_IMAGE_ASPECT_COLOR_BIT;
    image->last_batch = batch_serial;
    return 1;
}

static VkImageView image_view_from_semantic(const NvnDescriptorSnapshot* descriptor, u64 last_unmodeled_write) {
    u64 address;
    u32 width, height, levels;
    VkFormat format;
    if (!descriptor->texture || !descriptor->texture_generation ||
        !vk_tex_tic_2d(descriptor->tic, &address, &width, &height, &levels, &format))
        return VK_NULL_HANDLE;
    for (u32 i = 0; i < NVN_IMAGE_SLOTS; i++) {
        NvnImage* image = &images[i];
        if (!image->image || !image->sampled || image->target != 1 || image->layers != 1 ||
            image->texture != descriptor->texture || image->texture_generation != descriptor->texture_generation ||
            image->address != address || image->width != width || image->height != height ||
            (levels != image->levels && (levels != 1 || !image->base_view)))
            continue;
        VkFmt source = nvn_format(image->format);
        if (source.vk != format) continue;
        VkImageAspectFlags aspect = source.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        int complete = 1;
        for (u32 level = 0; level < levels; level++)
            if (!(image->mip_aspects[level] & aspect) || image->mip_sequence[level] <= last_unmodeled_write)
                complete = 0;
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
    int has_target = command->color_target_mask || command->depth_target_bound;
    if (!has_target || !command->target_complete) return NVN_DRAW_PREPARED | resource_status;
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
