/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what a window backend (platform/backend/) provides: the window, input and presenting */
#ifndef SNX_BACKEND_H
#define SNX_BACKEND_H
#include "cpu/aarch64/cpu.h"
#include "runtime/ini.h"

void video_init(const char* title);
void video_poll(void);
extern const IniOpt g_backend_ini[]; /* the backend's settings (touch) */
/* a key of the .ini or the command line for the backend, the on-screen controls or the unattended-run options:
   applied, 1; none of theirs, 0 */
int backend_ini(const char* k, const char* v);
/* the game's touch screen as the in-game menu shows and sets it: 0 off, 1 the fingers, 2 the mouse too. The set
   is saved to the .ini */
int backend_touch_mode(void);
void backend_touch_set(int mode);
void frontend_frame(void);
/* the window's size in pixels for the Vulkan swapchain, or 0 x 0 when headless */
void backend_window_size(int* w, int* h);
/* the part of the window the system's bars and the camera's cutout leave clear, in pixels: all of it when
   fullscreen, where the bars are hidden */
void backend_window_safe(int* x, int* y, int* w, int* h);
/* whether presenting can show anything now: not while Android has taken the window's surface away (the app in the
   background), nor while the window is minimized or covered. And a count that moves each time the surface comes back
   made anew, its Vulkan surface to be made anew too */
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
