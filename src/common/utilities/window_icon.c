/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "window_icon.h"

extern const unsigned char snx_window_icon_png[];
extern const size_t snx_window_icon_png_size;

SDL_Surface* snx_window_icon_surface(void) {
    return SDL_LoadPNG_IO(SDL_IOFromConstMem(snx_window_icon_png, snx_window_icon_png_size), true);
}

void snx_window_icon(SDL_Window* window) {
#ifdef _WIN32
    (void)window;
#else
    SDL_Surface* icon = snx_window_icon_surface();
    if (!icon) return;
    SDL_SetWindowIcon(window, icon); /* it keeps a copy */
    SDL_DestroySurface(icon);
#endif
}
