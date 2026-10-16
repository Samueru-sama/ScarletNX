/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "place.h"
#include <string.h>

static GuiRect rect(int x, int y, int w, int h) {
    GuiRect r;
    r.x = x;
    r.y = y;
    r.w = w > 0 ? w : 0;
    r.h = h > 0 ? h : 0;
    return r;
}
void gui_place(const GuiPlaceIn* in, GuiPlace* out) {
    int mx = in->cw, gap = in->ch / 2, width = in->width - 2 * mx, y, bottom, cols;
    GuiRect content;
    memset(out, 0, sizeof *out);
    out->edge_top = rect(0, 0, in->width, in->edge_top);
    out->edge_bottom = rect(0, in->height - in->edge_bottom, in->width, in->edge_bottom);
    y = in->edge_top + gap;
    if (in->tabs) {
        out->tabs = rect(mx, y, width, in->tabs);
        y += in->tabs + gap;
    }
    if (in->title) {
        out->title = rect(mx, y, width, in->title);
        y += in->title + gap;
    }
    bottom = in->height - in->edge_bottom - gap;
    if (in->bar) {
        out->bar = rect(mx, bottom - in->bar, width, in->bar);
        bottom -= in->bar + gap;
    }
    content = rect(mx, y, width, bottom - y);
    out->list = content;
    if (!in->list_cols) return;
    cols = content.w / in->cw;
    if (cols >= in->list_cols + 1 + GUI_PLACE_SIDE_MIN) {
        int w = in->list_cols * in->cw;
        out->list = rect(content.x, content.y, w, content.h);
        out->side = rect(content.x + w + in->cw, content.y, content.w - w - in->cw, content.h);
    } else { /* a phone held upright: the list over three fifths, the side box under it */
        int h = (content.h - gap) * 3 / 5;
        out->stacked = 1;
        out->list = rect(content.x, content.y, content.w, h);
        out->side = rect(content.x, content.y + h + gap, content.w, content.h - h - gap);
    }
    if (in->status) { /* at most half the column: the side box shows what the focus is about */
        int h = in->status < out->side.h / 2 ? in->status : out->side.h / 2;
        out->status = rect(out->side.x, out->side.y + out->side.h - h, out->side.w, h);
        out->side.h = out->side.h - h - gap > 0 ? out->side.h - h - gap : 0;
    }
}
int gui_place_list(int height, int item, int pitch, int count, int focus, int* first) {
    int fit = height >= item && pitch > 0 ? (height - item) / pitch + 1 : 0, shown = fit < count ? fit : count;
    *first = 0;
    if (shown > 0 && focus >= shown) *first = focus - shown + 1;
    if (*first > count - shown) *first = count - shown;
    if (*first < 0) *first = 0;
    return shown;
}
int gui_place_pitch(int height, int count, int least, int most) {
    int pitch = count > 0 ? height / count : most;
    return pitch < least ? least : pitch > most ? most : pitch;
}
