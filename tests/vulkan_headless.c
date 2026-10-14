/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Opt-in integration test: real renderer, no game data. Works with Mesa lavapipe and no display server. */
#include "common.h"
#include "runtime/main.h"
#include "runtime/options.h"
#include "gpu/renderer.h"
#include "gpu/vk/vk.h"
#include "memory/mem.h"
#include "runtime/platform/backend.h"
#include "spirv_emitter_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vulkan_headless:%d: %s\n", __LINE__, #x);                                                 \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int check_viewport_orientation(void) {
    u32 r[0xE00] = {0};
    float transform[6] = {32, 17, 0.5f, 40, 37, 0.5f};
    memcpy(&r[0x280], transform, sizeof transform);
    r[0x64B] = 1;
    r[0x286] = 0x6420; /* identity x/y/z/w swizzle */
    r[0x647] = 0x900;  /* guest clockwise */
    VkViewport vp = vk_render_viewport(r);
    CHECK(vp.x == 8 && vp.width == 64 && vp.y == 20 && vp.height == 34);
    CHECK(vp.minDepth == 0 && vp.maxDepth == 1);
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    /* NegativeY reverses the endpoints about translate_y, keeping the same viewport rectangle. */
    r[0x286] = 0x6430;
    vp = vk_render_viewport(r);
    CHECK(vp.y == 54 && vp.height == -34 && vp.y + vp.height == 20);
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    /* Negative scale and negative coordinate swizzle cancel. Neither changes the face-control register. */
    transform[1] = -17;
    memcpy(&r[0x280], transform, sizeof transform);
    vp = vk_render_viewport(r);
    CHECK(vp.y == 20 && vp.height == 34);
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    r[0x4EB] = 0x10;
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    r[0x647] = 0x901;
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    return 0;
}

/* Execute the direct writer's module, not only spirv-val's structural check. */
static int check_direct_spirv(void) {
    size_t words = 0;
    u32* code = snx_test_spirv_compute(&words);
    VkShaderModuleCreateInfo smi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * sizeof(u32), .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(g_vk.dev, &smi, NULL, &module) == VK_SUCCESS);
    free(code);
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout dlayout;
    CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &dlayout) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dlayout};
    VkPipelineLayout playout;
    CHECK(vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &playout) == VK_SUCCESS);
    VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                 .module = module,
                                                 .pName = "main"},
                                       .layout = playout};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline) == VK_SUCCESS);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = sizeof(u32),
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer;
    VmaAllocation memory;
    VmaAllocationInfo allocation;
    CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buffer, &memory, &allocation) == VK_SUCCESS);
    *(u32*)allocation.pMappedData = 0;
    CHECK(vmaFlushAllocation(g_vk.vma, memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &pool_size};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &pool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo dsi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = pool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &dlayout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(g_vk.dev, &dsi, &set) == VK_SUCCESS);
    VkDescriptorBufferInfo dbi = {buffer, 0, sizeof(u32)};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(g_vk.dev, 1, &write, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(g_vk.dev, &cpi, NULL, &cpool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = cpool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(g_vk.dev, &cbi, &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &dependency);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(g_vk.gfx, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(vkQueueWaitIdle(g_vk.gfx) == VK_SUCCESS);
    CHECK(vmaInvalidateAllocation(g_vk.vma, memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    CHECK(*(u32*)allocation.pMappedData == 42);
    vkDestroyCommandPool(g_vk.dev, cpool, NULL);
    vkDestroyDescriptorPool(g_vk.dev, pool, NULL);
    vmaDestroyBuffer(g_vk.vma, buffer, memory);
    vkDestroyPipeline(g_vk.dev, pipeline, NULL);
    vkDestroyPipelineLayout(g_vk.dev, playout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, dlayout, NULL);
    return 0;
}

int main(int argc, char** argv) {
    const u32 w = 32, h = 16;
    const u64 address = 4096;
    const i32 full[4] = {0, 0, 0, 0}, crop[4] = {4, 2, 20, 10};
    char *root = snx_temp_dir(NULL), *shot, *ppm;
    CHECK(root);
    CHECK(!snx_startup_options(argc, argv));
    snprintf(g_exedir, 1024, "%s/", root);
    shot = snx_path_join(root, "frame");
    ppm = snx_format("%s.ppm", shot);
    g_mem = calloc(1, (size_t)address + w * h * 4);
    CHECK(g_mem);
    video_init("Headless Vulkan fixture");
    CHECK(g_opt_headless && g_renderer_up && g_vk.dev && !g_vk.surface && !g_vk.swapchain && !g_vk.sc_count);
    CHECK(!check_viewport_orientation());
    CHECK(!check_direct_spirv());
    printf("Headless Vulkan device: %s\n", g_vk.props.deviceName);
    /* Make any accidental WSI use during presents/recreation fail the test immediately. */
    vkCreateSwapchainKHR = NULL;
    vkAcquireNextImageKHR = NULL;
    vkQueuePresentKHR = NULL;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR = NULL;
    CHECK(!vk_swapchain_create(0));
    u32* pixels = (u32*)(g_mem + address);
    for (u32 frame = 0; frame < 8; ++frame) {
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) pixels[y * w + x] = 0xff000000u | x | (y << 8) | (frame << 16);
        const i32* region = frame & 1 ? crop : full;
        u32 x0 = frame & 1 ? 4 : 0, y0 = frame & 1 ? 2 : 0;
        u32 width = frame & 1 ? 16 : w, height = frame & 1 ? 8 : h;
        u32 transform = frame >> 1; /* none, horizontal, vertical, both; each with a full image and a crop */
        u64 expected = 1469598103934665603ull;
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                u32 sx = x0 + ((transform & 1) ? width - 1 - x : x);
                u32 sy = y0 + ((transform & 2) ? height - 1 - y : y);
                expected = (expected ^ pixels[sy * w + sx]) * 1099511628211ull;
            }
        u64 before = vk_frame_number();
        g_renderer->present_buffer(address, w, h, w * 4, 0, 0, 1, region, transform);
        CHECK(vk_frame_number() == before + 1);
        CHECK(!g_vk.surface && !g_vk.swapchain);
        CHECK(g_renderer->screenshot(shot) == expected);
        size_t size;
        unsigned char* data = snx_read_file(ppm, &size);
        char header[64];
        int n = snprintf(header, sizeof header, "P6\n%u %u\n255\n", width, height);
        CHECK(data && size == (size_t)n + width * height * 3 && !memcmp(data, header, (size_t)n));
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                unsigned char* p = data + n + (y * width + x) * 3;
                CHECK(p[0] == x0 + ((transform & 1) ? width - 1 - x : x));
                CHECK(p[1] == y0 + ((transform & 2) ? height - 1 - y : y));
                CHECK(p[2] == frame);
            }
        free(data);
    }
    /* Even a present without an image submits and retires the current frame slot. */
    u64 before = vk_frame_number();
    g_renderer->present_buffer(0, 0, 0, 0, 0, 0, 1, full, 0);
    CHECK(vk_frame_number() == before + 1);
    vk_finish();
    free(g_mem);
    g_mem = NULL;
    CHECK(!snx_remove_tree(root));
    free(root);
    free(shot);
    free(ppm);
    puts("PASS headless Vulkan: viewport orientation, 8 cropped/transformed readbacks and PPMs, no WSI");
    return 0;
}
