/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The window icon built in: dist/icons' 128 PNG byte for byte, decoded to the file's pixels, and a window takes it */
#include "window_icon.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern const unsigned char snx_window_icon_png[];
extern const size_t snx_window_icon_png_size;

int main(int argc, char** argv) {
    size_t size;
    void* bytes;
    SDL_Surface* icon;
    SDL_Surface* file;
    int drawn, y, x;
    Uint8 r, g, b, a;
    SDL_Window* window;
    (void)argc;
    (void)argv;
    assert(SDL_Init(SDL_INIT_VIDEO));
    size = 0;
    bytes = SDL_LoadFile(SNX_WINDOW_ICON_FILE, &size);
    assert(bytes && size == snx_window_icon_png_size && !memcmp(bytes, snx_window_icon_png, size));
    SDL_free(bytes);
    icon = snx_window_icon_surface();
    assert(icon && icon->w == 128 && icon->h == 128);
    file = SDL_LoadPNG(SNX_WINDOW_ICON_FILE);
    assert(file && file->w == icon->w && file->h == icon->h);
    drawn = 0;
    for (y = 0; y < icon->h; ++y)
        for (x = 0; x < icon->w; ++x) {
            Uint8 r = 0, g = 0, b = 0, a = 0, fr = 0, fg = 0, fb = 0, fa = 0;
            assert(SDL_ReadSurfacePixel(icon, x, y, &r, &g, &b, &a));
            assert(SDL_ReadSurfacePixel(file, x, y, &fr, &fg, &fb, &fa));
            assert(r == fr && g == fg && b == fb && a == fa);
            drawn += a == 255;
        }
    /* a drawing, not an empty or a filled square: the corners clear, a good part of it opaque */
    r = g = b = 0;
    a = 255;
    assert(SDL_ReadSurfacePixel(icon, 0, 0, &r, &g, &b, &a) && a == 0);
    assert(drawn > 128 * 128 / 5 && drawn < 128 * 128 * 4 / 5);
    SDL_DestroySurface(file);
    SDL_DestroySurface(icon);
    window = SDL_CreateWindow("icon", 64, 64, SDL_WINDOW_HIDDEN);
    assert(window);
    snx_window_icon(window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    printf("window icon: 128x128, %d of %d pixels opaque, the file's pixels\n", drawn, 128 * 128);
    return 0;
}
