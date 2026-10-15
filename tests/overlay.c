/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu (runtime/platform/overlay.c): where it goes, its pages, what a tap, a move or a step does, its look;
   and ui.c's atlas and text */
#include "runtime/platform/overlay.h"
#include "runtime/platform/vpad.h"
#include "runtime/platform/controls_atlas.h"
#include "runtime/platform/plat.h"
#include "font.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform's lock and the backend's touch screen, without either */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
static int touch = 1;
int backend_touch_mode(void) { return touch; }
void backend_touch_set(int mode) { touch = mode; }
static char stored[64];
static void store(const char* key, const char* value) {
    (void)value;
    snprintf(stored, sizeof stored, "%s", key);
}

enum { SW = 2424, SH = 1080 };
static int px, py, s;
/* a tap on a row of items, at a column, where the panel is for its page */
static int tap(int row, int col) {
    overlay_place(SW, SH, &px, &py, &s);
    return overlay_tap((float)(px + col * 8 * s + 2), (float)(py + (3 + row) * 16 * s + 5), SW, SH);
}
static SnxUiQuad quads[2048];
static SnxUiList list;
static void draw(void) {
    overlay_place(SW, SH, &px, &py, &s);
    list.q = quads;
    list.n = 0;
    list.max = 2048;
    overlay_draw(&list, SW, SH);
}
/* the quad of a glyph at a cell, or -1 */
static int glyph_at(int col, int row, u8 c) {
    int i;
    for (i = 0; i < list.n; ++i)
        if (list.q[i].x == (float)(px + col * 8 * s) && list.q[i].y == (float)(py + row * 16 * s) &&
            list.q[i].u == SNX_CONTROLS_FONT_X + c % 16 * 8 && list.q[i].v == SNX_CONTROLS_FONT_Y + c / 16 * 16)
            return i;
    return -1;
}
/* whether a line of text is drawn from that cell on, in the font's glyphs (a space draws none) */
static int text_at(int col, int row, const char* text) {
    int k;
    for (k = 0; text[k]; ++k)
        if (text[k] != ' ' && glyph_at(col + k, row, (u8)text[k]) < 0) return 0;
    return 1;
}

int main(void) {
    const u32 ink = 0xFF3A4145u;
    int x, y, sc;
    vpad_set_store(store);
    assert(!overlay_shown());
    overlay_toggle();
    assert(overlay_shown());
    /* half the screen at most, in whole steps, in the middle: a phone held wide, a small window. The page is as
       tall as its items (two here, under three rows of head, over a blank one), at the scale of the tallest page */
    overlay_place(SW, SH, &px, &py, &s);
    assert(s == 2 && px == (SW - 640) / 2 && py == (SH - 6 * 32) / 2);
    overlay_place(500, 100, &x, &y, &sc);
    assert(sc == 1 && x == 90 && y == 2);

    /* the first page: the title on a band of ink, the items under it, the focused one on a band */
    draw();
    assert(list.q[0].w == 640 && list.q[0].h == 6 * 32 && list.q[1].rgba == ink && list.q[1].h == 32);
    assert(text_at(38, 3, ">")); /* a page under it */
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && text_at(1, 3, "\xFE On-screen controls") &&
           text_at(1, 4, "\xFE Exit"));
    /* a tap on Exit exits; on the title, nothing; outside the panel, it closes */
    assert(tap(1, 3) == OVERLAY_EXIT && tap(-3, 3) == OVERLAY_NONE);
    assert(overlay_tap(5.0f, 5.0f, SW, SH) == OVERLAY_CLOSE);
    /* the focus goes round, from Exit to the first item and back over the top; Back from here closes */
    overlay_move(1);
    overlay_move(-3);
    assert(overlay_select() == OVERLAY_EXIT && overlay_back() == OVERLAY_CLOSE);

    /* the controls' page: each setting shown with its value, changed and kept */
    assert(tap(0, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLS") && text_at(1, 3, "\xFE Show controls"));
    assert(list.q[0].h == 16 * 32 && text_at(38, 8, ">")); /* twelve items: the tallest; Buttons opens a page */
    vpad_config()->on = 0;
    draw();
    assert(text_at(36, 3, "Off"));
    assert(tap(0, 3) == OVERLAY_NONE && vpad_config()->on && !strcmp(stored, "controls"));
    draw();
    assert(text_at(37, 3, "On"));
    /* Scale's arrows: the left one down, the right one up, by tens, between 50% and 150% */
    assert(vpad_config()->scale == 50);
    draw();
    assert(text_at(31, 6, "< 100% >"));
    tap(3, 38);
    assert(vpad_config()->scale == 60 && !strcmp(stored, "controls_scale"));
    tap(3, 31);
    tap(3, 31);
    assert(vpad_config()->scale == 40);
    for (x = 0; x < 9; ++x) tap(3, 31);
    assert(vpad_config()->scale == 0);
    vpad_config()->scale = 50;
    /* the keys: down from Scale to Opacity, left steps it */
    overlay_move(1);
    overlay_side(-1);
    assert(vpad_config()->opacity == 90 && !strcmp(stored, "controls_opacity"));
    overlay_side(1);
    /* the toggles; the touch screen goes round its three */
    tap(2, 3);
    assert(vpad_config()->snap && !strcmp(stored, "controls_snap"));
    tap(2, 3);
    tap(6, 3);
    assert(!vpad_config()->rel_center && !strcmp(stored, "controls_rel_center"));
    tap(6, 3);
    tap(9, 3);
    assert(touch == 2);
    tap(9, 3);
    assert(touch == 0);
    draw();
    assert(text_at(36, 12, "Off"));
    tap(9, 3);
    /* Edit layout is the caller's to do */
    assert(tap(1, 3) == OVERLAY_EDIT);

    /* the buttons' page: one hidden and kept, All shows them again; it scrolls to its last item */
    assert(tap(5, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  BUTTONS") && text_at(1, 4, "\xFE L"));
    tap(1, 3);
    assert(!vpad_config()->at[SNX_VPAD_L].shown && !strcmp(stored, "controls_l"));
    draw();
    assert(text_at(33, 4, "Hidden") && text_at(28, 3, "Some hidden"));
    tap(0, 3);
    assert(vpad_config()->at[SNX_VPAD_L].shown);
    tap(0, 3); /* all shown: All hides them */
    assert(!vpad_config()->at[SNX_VPAD_A].shown && !vpad_config()->at[SNX_VPAD_L].shown);
    tap(0, 3);
    overlay_move(-1); /* round to Back, at the bottom */
    draw();
    assert(text_at(1, 3 + 11, "\xFE Back") && text_at(39, 3, "\x18") && !text_at(39, 14, "\x19"));
    /* Back on it, and the key Back, go up a page each */
    assert(overlay_select() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLS"));
    assert(overlay_back() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));
    /* closed, it draws nothing; opened again, its first page */
    overlay_toggle();
    draw();
    assert(!overlay_shown() && !list.n);
    overlay_toggle();
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));

    /* ui.c: the atlas with the font's glyphs, white where a glyph's bit is, and sprites' alpha premultiplied */
    {
        u32* sprites = calloc((size_t)SNX_CONTROLS_ATLAS_WIDTH * SNX_CONTROLS_ATLAS_HEIGHT, 4);
        const u32* a;
        int w, h;
        u32 version;
        assert(sprites);
        sprites[0] = 0x80FF8040u; /* half alpha */
        assert(!ui_atlas(&w, &h, &version) && !version);
        ui_atlas_set(sprites, 4, 4); /* not the atlas the header is for */
        assert(!ui_atlas(&w, &h, &version));
        ui_atlas_set(sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT);
        a = ui_atlas(&w, &h, &version);
        assert(a && version == 1 && w == SNX_CONTROLS_ATLAS_WIDTH);
        assert(a[0] == 0x80804020u);
        /* every glyph, white where its bit is: 'A' has both */
        for (sc = 0; sc < 256; ++sc)
            for (y = 0; y < 16; ++y)
                for (x = 0; x < 8; ++x)
                    assert(a[(SNX_CONTROLS_FONT_Y + sc / 16 * 16 + y) * w + SNX_CONTROLS_FONT_X + sc % 16 * 8 + x] ==
                           (gui_vga_font[sc * 16 + y] & (0x80 >> x) ? 0xFFFFFFFFu : 0));
        assert(gui_vga_font['A' * 16 + 5] && gui_vga_font['A' * 16 + 5] != 0xFF);
        assert(a[(SNX_CONTROLS_WHITE_Y + 1) * w + SNX_CONTROLS_WHITE_X + 1] == 0xFFFFFFFFu);
        ui_atlas_done();
        assert(!ui_atlas(&w, &h, &version) && version == 1);
        free(sprites);
    }
    puts("overlay: placing, pages, taps, keys, values, scrolling, look and the atlas passed");
    return 0;
}
