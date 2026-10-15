/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/platform/ui.h"
#include "runtime/platform/controls_atlas.h"
#include "runtime/platform/plat.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>

static PMutex g_lock = PMUTEX_INIT;
static u32* g_atlas;
static u32 g_version;

void ui_lock(void) { pm_lock(&g_lock); }
void ui_unlock(void) { pm_unlock(&g_lock); }

static void quad(SnxUiList* l, float x, float y, float w, float h, int u, int v, int uw, int vh, u32 rgba, int turn,
                 int sharp) {
    SnxUiQuad* q;
    if (l->n >= l->max || w <= 0 || h <= 0) return;
    q = &l->q[l->n++];
    q->x = x;
    q->y = y;
    q->w = w;
    q->h = h;
    q->u = (u16)u;
    q->v = (u16)v;
    q->uw = (u16)uw;
    q->vh = (u16)vh;
    q->rgba = rgba;
    q->turn = (u8)(turn & 3);
    q->sharp = (u8)sharp;
}
void ui_sprite(SnxUiList* l, int sprite, float x, float y, float w, float h, u32 alpha, int turn) {
    const unsigned short* s = snx_controls_sprites[sprite];
    if (alpha) quad(l, x, y, w, h, s[0], s[1], s[2], s[3], 0x00FFFFFFu | alpha << 24, turn, 0);
}
void ui_fill(SnxUiList* l, float x, float y, float w, float h, u32 rgba) {
    /* the block's inner texels: a blend at its edge never reaches the transparent ones around it */
    quad(l, x, y, w, h, SNX_CONTROLS_WHITE_X + 1, SNX_CONTROLS_WHITE_Y + 1, 2, 2, rgba, 0, 1);
}
float ui_text(SnxUiList* l, float x, float y, int scale, const char* s, u32 rgba) {
    float x0 = x;
    for (; *s; ++s, x += 8.0f * (float)scale) {
        u8 c = (u8)*s;
        if (c != ' ')
            quad(l, x, y, 8.0f * (float)scale, 16.0f * (float)scale, SNX_CONTROLS_FONT_X + c % 16 * 8,
                 SNX_CONTROLS_FONT_Y + c / 16 * 16, 8, 16, rgba, 0, 1);
    }
    return x - x0;
}

void ui_atlas_set(const u32* sprites, int w, int h) {
    int x, y, c, i;
    u32* a;
    if (w != SNX_CONTROLS_ATLAS_WIDTH || h != SNX_CONTROLS_ATLAS_HEIGHT) return; /* not the image the header is for */
    a = (u32*)malloc((size_t)w * (size_t)h * 4);
    if (!a) return;
    for (i = 0; i < w * h; ++i) { /* premultiplied: a blend between a sprite's edge and the clear texels around it */
        u32 p = sprites[i], al = p >> 24;
        a[i] = ((p & 0xFFu) * al / 255) | ((p >> 8 & 0xFFu) * al / 255) << 8 | ((p >> 16 & 0xFFu) * al / 255) << 16 |
               al << 24;
    }
    for (c = 0; c < 256; ++c)
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 8; ++x)
                a[(SNX_CONTROLS_FONT_Y + c / 16 * 16 + y) * w + SNX_CONTROLS_FONT_X + c % 16 * 8 + x] =
                    gui_vga_font[c * 16 + y] & (0x80 >> x) ? 0xFFFFFFFFu : 0;
    for (y = 0; y < 4; ++y)
        for (x = 0; x < 4; ++x) a[(SNX_CONTROLS_WHITE_Y + y) * w + SNX_CONTROLS_WHITE_X + x] = 0xFFFFFFFFu;
    ui_lock();
    free(g_atlas);
    g_atlas = a;
    ++g_version;
    ui_unlock();
}
const u32* ui_atlas(int* w, int* h, u32* version) {
    *w = SNX_CONTROLS_ATLAS_WIDTH;
    *h = SNX_CONTROLS_ATLAS_HEIGHT;
    *version = g_version;
    return g_atlas;
}
void ui_atlas_done(void) {
    free(g_atlas);
    g_atlas = NULL;
}
