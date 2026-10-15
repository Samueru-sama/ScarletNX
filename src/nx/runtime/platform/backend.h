/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what a window backend (platform/backend/) provides: the window, input and presenting */
#ifndef SNX_BACKEND_H
#define SNX_BACKEND_H
#include "cpu/aarch64/cpu.h"

void video_init(const char* title);
void video_present_rgba(const u32* px, int w, int h, int stride); /* final RGBA8 image */
void video_poll(void);
void backend_ini(const char* k, const char* v);
void frontend_frame(void);
/* the window's size in pixels for the Vulkan swapchain, or 0 x 0 when headless */
void backend_window_size(int* w, int* h);
/* the part of the window the system's bars and the camera's cutout leave clear, in pixels: all of it when
   fullscreen, where the bars are hidden */
void backend_window_safe(int* x, int* y, int* w, int* h);
/* Android takes the window's surface away while the app is in the background: whether it is there now, and a count
   that moves each time it comes back made anew, its Vulkan surface to be made anew too */
int backend_window_shown(void);
u32 backend_window_generation(void);
/* fatal's error box, on every platform despite the name (a no-op where the backend has none), from any thread */
void win_fatal_box(const char* msg);

/* The Vulkan renderer's window side, unused when headless. The handles as vulkan_core.h defines them on
   64-bit hosts (C allows the same typedef twice), so including this needs no Vulkan header. */
typedef struct VkInstance_T* VkInstance;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
void* backend_vk_loader(void); /* vkGetInstanceProcAddr */
const char* const* backend_vk_extensions(u32* n);
int backend_vk_surface(VkInstance instance, VkSurfaceKHR* surface);

#endif
