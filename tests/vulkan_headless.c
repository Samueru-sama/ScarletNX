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

/* VIC through the compute shader: raw YUV and alpha, odd starts kept, luma-only NV12, bytes outside untouched */
static int check_vic(void) {
    enum { W = 32, H = 16, SENTINEL = 0xA5 };
    static u8 y[W * H], uv[W * H / 2], out[W * 4 * H];
    for (u32 j = 0; j < H; ++j)
        for (u32 i = 0; i < W; ++i) y[j * W + i] = (u8)(16 + i + 4 * j);
    for (u32 j = 0; j < H / 2; ++j)
        for (u32 i = 0; i < W / 2; ++i) {
            uv[j * W + i * 2] = (u8)(40 + i + 8 * j);
            uv[j * W + i * 2 + 1] = (u8)(240 - i - 8 * j);
        }
    static const struct {
        u32 fmt, sx0, sy0, dx0, dy0, w, h;
    } cases[] = {{0x1F, 0, 0, 0, 0, W, H}, {0x1F, 3, 1, 5, 3, 10, 7}, {0x44, 0, 0, 0, 0, W, H}};
    for (u32 c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const int rgba = cases[c].fmt != 0x44;
        const u32 bpp = rgba ? 4 : 1;
        VicJob job;
        memset(&job, 0, sizeof job);
        memset(out, SENTINEL, sizeof out);
        job.fmt = cases[c].fmt;
        job.sw = W;
        job.sh = H;
        job.oy = (VicPlane){out, (u64)W * bpp * H, W * bpp, H, 0, 0};
        VicSlot* sl = &job.slots[0];
        sl->on = 1;
        sl->sx0 = cases[c].sx0;
        sl->sy0 = cases[c].sy0;
        sl->dx0 = cases[c].dx0;
        sl->dy0 = cases[c].dy0;
        sl->w = cases[c].w;
        sl->h = cases[c].h;
        sl->alpha = 0x3FF;
        sl->clamp_hi = 0x3FF;
        sl->iy = (VicPlane){y, sizeof y, W, H, 0, 0};
        sl->ic = (VicPlane){uv, sizeof uv, W, H / 2, 0, 0};
        CHECK(g_renderer->vic_compose(&job));
        for (u32 j = 0; j < H; ++j)
            for (u32 i = 0; i < W; ++i) {
                const u8* o = out + (j * W + i) * bpp;
                const int in = i >= sl->dx0 && i < sl->dx0 + sl->w && j >= sl->dy0 && j < sl->dy0 + sl->h;
                const u32 sx = sl->sx0 + i - sl->dx0, sy = sl->sy0 + j - sl->dy0;
                if (!in) {
                    CHECK(o[0] == SENTINEL);
                } else if (!rgba) {
                    CHECK(o[0] == y[sy * W + sx]);
                } else {
                    const u8* p = uv + (sy / 2) * W + (sx & ~1u);
                    CHECK(o[0] == y[sy * W + sx] && o[1] == p[0] && o[2] == p[1] && o[3] == 255);
                }
            }
    }
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
    CHECK(!check_vic());
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
    puts("PASS headless Vulkan: viewport orientation, 3 VIC compositions, 8 cropped/transformed readbacks and PPMs, "
         "no WSI");
    return 0;
}
