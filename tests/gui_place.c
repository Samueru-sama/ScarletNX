/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The boxes of a screen in boxes (place.c): where each goes on a computer's window and on a phone held upright, and how
   a list of items fits its box. */
#include "place.h"
#include <assert.h>
#include <stdio.h>

static int empty(GuiRect r) { return !r.w || !r.h; }
static int inside(GuiRect r, int width, int height) {
    return r.x >= 0 && r.y >= 0 && r.x + r.w <= width && r.y + r.h <= height;
}
static int apart(GuiRect a, GuiRect b) {
    return empty(a) || empty(b) || a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y;
}
/* every box in the area, none over another */
static void sound(const GuiPlace* p, int width, int height) {
    const GuiRect all[] = {p->edge_top, p->edge_bottom, p->tabs, p->title, p->list, p->side, p->status, p->bar};
    size_t i, j, n = sizeof all / sizeof *all;
    for (i = 0; i < n; ++i) {
        assert(inside(all[i], width, height));
        for (j = i + 1; j < n; ++j) assert(apart(all[i], all[j]));
    }
}

int main(void) {
    GuiPlaceIn in = {1280, 800, 16, 32, 24, 24, 1, 64, 72, 34, 96};
    GuiPlace p;
    int first;
    /* a computer's window: the edges on the whole width, the rest a column in from either side, top to bottom tabs,
       title, the list and the box beside it, the bar; the status box at the foot of the side's column */
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.edge_top.y == 0 && p.edge_top.w == 1280 && p.edge_top.h == 24);
    assert(p.edge_bottom.y == 776 && p.edge_bottom.h == 24);
    assert(p.tabs.x == 16 && p.tabs.w == 1248 && p.tabs.y == 24 + 16 && p.tabs.h == 32);
    assert(p.title.y == p.tabs.y + 32 + 16 && p.title.h == 64);
    assert(p.list.y == p.title.y + 64 + 16 && p.list.x == 16 && p.list.w == 34 * 16);
    assert(!p.stacked && p.side.y == p.list.y && p.side.x == p.list.x + p.list.w + 16);
    assert(p.side.x + p.side.w == 1280 - 16);
    assert(p.bar.y + p.bar.h == 776 - 16 && p.bar.h == 72 && p.list.y + p.list.h == p.bar.y - 16);
    assert(p.status.h == 96 && p.status.y + p.status.h == p.list.y + p.list.h && p.status.x == p.side.x);
    assert(p.side.y + p.side.h == p.status.y - 16);
    /* nothing asked for, nothing given: no edges, tabs, title or bar, half a row from the top and the bottom */
    {
        GuiPlaceIn bare = {640, 400, 8, 16, 0, 0, 0, 0, 0, 30, 0};
        gui_place(&bare, &p);
        sound(&p, 640, 400);
        assert(empty(p.edge_top) && empty(p.tabs) && empty(p.title) && empty(p.bar) && empty(p.status));
        assert(p.list.y == 8 && p.list.h == 400 - 16 && p.side.h == p.list.h);
    }
    /* one box, the whole width: no side box */
    in.list_cols = 0;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.list.w == 1248 && empty(p.side) && empty(p.status));
    /* a phone held upright: too narrow for the list and a side box of GUI_PLACE_SIDE_MIN columns, so the side box goes
       under the list, the status box under it */
    {
        GuiPlaceIn phone = {720, 1500, 24, 48, 36, 36, 1, 96, 120, 20, 144};
        assert(720 / 24 - 2 < 20 + 1 + GUI_PLACE_SIDE_MIN);
        gui_place(&phone, &p);
        sound(&p, 720, 1500);
        assert(p.stacked && p.list.w == 720 - 48 && p.side.w == p.list.w && p.side.y > p.list.y + p.list.h);
        assert(p.list.h > p.side.h && p.status.y > p.side.y + p.side.h);
    }
    /* a status box taller than its column takes half of it, the side box the rest */
    in.list_cols = 34;
    in.status = 2000;
    gui_place(&in, &p);
    sound(&p, 1280, 800);
    assert(p.status.h == p.list.h / 2 && p.side.h == p.list.h - p.status.h - 16);
    /* a window too small for its bar: boxes of no size, never of a negative one */
    {
        GuiPlaceIn tiny = {100, 60, 8, 16, 24, 24, 1, 32, 72, 10, 16};
        gui_place(&tiny, &p);
        assert(p.list.h == 0 && p.list.w >= 0 && p.side.w >= 0 && p.status.h == 0);
    }
    /* a list: as many items as fit, the focus among them, from the first while it is in view */
    assert(gui_place_list(100, 20, 30, 10, -1, &first) == 3 && first == 0);
    assert(gui_place_list(100, 20, 30, 10, 2, &first) == 3 && first == 0);
    assert(gui_place_list(100, 20, 30, 10, 5, &first) == 3 && first == 3);
    assert(gui_place_list(100, 20, 30, 10, 9, &first) == 3 && first == 7);
    assert(gui_place_list(100, 20, 30, 2, 1, &first) == 2 && first == 0);
    assert(gui_place_list(10, 20, 30, 5, 3, &first) == 0 && first == 0); /* not even one fits */
    /* the pitch spreads few items, keeps many to the least */
    assert(gui_place_pitch(300, 10, 20, 50) == 30);
    assert(gui_place_pitch(300, 2, 20, 50) == 50 && gui_place_pitch(300, 30, 20, 50) == 20);
    assert(gui_place_pitch(300, 0, 20, 50) == 50);
    puts("gui place ok");
    return 0;
}
