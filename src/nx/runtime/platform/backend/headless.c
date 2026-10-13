/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* No-window backend: Vulkan offscreen rendering when enabled, otherwise the null renderer.
   Both use the unattended-run options (platform/unattended.c). */
#include "runtime/main.h"
#include "gpu/renderer.h"
#include "hle/services/hid.h"
#include "runtime/platform/backend.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include "runtime/platform/unattended.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef SNX_VULKAN
static u32* g_last;
static int g_lw, g_lh;
static PMutex g_hl;
#endif
PAudio* plat_audio_open(u32 rate, u32 channels) {
    (void)rate;
    (void)channels;
    return NULL;
}
void plat_audio_close(PAudio* a) { (void)a; }
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes) {
    (void)a;
    (void)samples;
    (void)bytes;
    return 0;
}
void plat_audio_start(PAudio* a, int started) {
    (void)a;
    (void)started;
}
void plat_audio_clear(PAudio* a) { (void)a; }
u32 plat_audio_queued(PAudio* a) {
    (void)a;
    return 0;
}
void plat_audio_volume(PAudio* a, float volume) {
    (void)a;
    (void)volume;
}
void backend_ini(const char* k, const char* v) { unattended_ini(k, v); }
void video_init(const char* t) {
    (void)t;
#ifndef SNX_VULKAN
    pm_init(&g_hl);
#endif
    unattended_start();
#ifdef SNX_VULKAN
    if (!g_renderer->start()) fatal("Headless Vulkan could not start (see the log). Vulkan 1.3 is required.");
    LOG("renderer: Vulkan headless (no window, surface or swapchain)\n");
#endif
}
void backend_window_size(int* w, int* h) { *w = *h = 0; }
void* backend_vk_loader(void) { return NULL; } /* volk loads Vulkan directly */
const char* const* backend_vk_extensions(u32* n) {
    *n = 0;
    return NULL;
}
int backend_vk_surface(VkInstance instance, VkSurfaceKHR* surface) {
    (void)instance;
    (void)surface;
    return 0;
}
#ifdef SNX_VULKAN
void video_present_rgba(const u32* px, int w, int h, int stride) {
    /* Vulkan's present_buffer retains the offscreen image for screenshots. */
    (void)px;
    (void)w;
    (void)h;
    (void)stride;
}
#else
static void save_ppm(const char* path) {
    if (!g_last) return;
    FILE* f = fopen(path, "wb");
    if (!f) {
        WARN("screenshot: could not write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", g_lw, g_lh);
    for (int i = 0; i < g_lw * g_lh; i++) {
        u32 p = g_last[i];
        u8 rgb[3] = {(u8)p, (u8)(p >> 8), (u8)(p >> 16)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
void video_present_rgba(const u32* px, int w, int h, int stride) {
    pm_lock(&g_hl);
    if (g_lw != w || g_lh != h) {
        free(g_last);
        g_last = malloc((size_t)w * h * 4);
        g_lw = g_last ? w : 0;
        g_lh = g_last ? h : 0;
    }
    if (g_last) {
        for (int y = 0; y < h; y++) memcpy(g_last + (size_t)y * w, px + (size_t)y * stride, (size_t)w * 4);
        char p[1100];
        if (unattended_periodic_shot(p, sizeof p - 4)) {
            strcat(p, ".ppm");
            save_ppm(p);
        }
    }
    pm_unlock(&g_hl);
}
#endif
void video_poll(void) {
    /* the scripted presses, stick keys (40..47) as axes like the SDL3 backend's keyboard */
    u64 s = unattended_buttons();
    i32 st[4];
    input_key_sticks(s, st);
    input_stick_clamp(&st[0], &st[1]);
    input_stick_clamp(&st[2], &st[3]);
    g_pad.buttons = s & 0xFFFFFFull;
    g_pad.lx = st[0];
    g_pad.ly = st[1];
    g_pad.rx = st[2];
    g_pad.ry = st[3];
}
void frontend_frame(void) {
#ifdef SNX_VULKAN
    char path[1200];
    if (unattended_periodic_shot(path, sizeof path)) g_renderer->screenshot(path);
    if (unattended_done()) unattended_exit(g_renderer->screenshot(unattended_shot()), 0, 0);
#else
    if (unattended_done()) {
        pm_lock(&g_hl);
        const char* shot = unattended_shot();
        if (shot) {
            char p[1100];
            snprintf(p, sizeof p, "%s.ppm", shot);
            save_ppm(p);
        }
        u64 h = 1469598103934665603ull;
        if (g_last)
            for (int i = 0; i < g_lw * g_lh; i++) {
                h ^= g_last[i];
                h *= 1099511628211ull;
            }
        int w = g_lw, hh = g_lh;
        pm_unlock(&g_hl);
        unattended_exit(h, w, hh);
    }
#endif
}
/* No dialogs in a backend that deliberately has no window. */
void win_fatal_box(const char* msg) { (void)msg; }
