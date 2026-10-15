/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/platform/touch.h"

void touch_picture(int cx, int cy, int cw, int ch, int picture[4]) {
    int w = cw, h = (int)((i64)cw * SNX_HID_TOUCH_HEIGHT / SNX_HID_TOUCH_WIDTH);
    if (h > ch) {
        h = ch;
        w = (int)((i64)ch * SNX_HID_TOUCH_WIDTH / SNX_HID_TOUCH_HEIGHT);
    }
    picture[0] = cx + (cw - w) / 2;
    picture[1] = cy + (ch - h) / 2;
    picture[2] = w;
    picture[3] = h;
}
static int slot_of(const TouchState* t, const u64* ids, u64 id) {
    int i;
    for (i = 0; i < SNX_HID_TOUCHES; ++i)
        if ((t->down >> i & 1) && ids[i] == id) return i;
    return -1;
}
static void place(TouchState* t, int i, const int picture[4], float px, float py) {
    float x = (px - (float)picture[0]) * SNX_HID_TOUCH_WIDTH / (float)picture[2];
    float y = (py - (float)picture[1]) * SNX_HID_TOUCH_HEIGHT / (float)picture[3];
    t->x[i] = x < 0 ? 0 : x >= SNX_HID_TOUCH_WIDTH ? SNX_HID_TOUCH_WIDTH - 1 : (u32)x;
    t->y[i] = y < 0 ? 0 : y >= SNX_HID_TOUCH_HEIGHT ? SNX_HID_TOUCH_HEIGHT - 1 : (u32)y;
}
int touch_down(TouchState* t, u64* ids, u64 id, const int picture[4], float px, float py) {
    int i = slot_of(t, ids, id);
    if (picture[2] <= 0 || picture[3] <= 0 || px < (float)picture[0] || py < (float)picture[1] ||
        px >= (float)(picture[0] + picture[2]) || py >= (float)(picture[1] + picture[3]))
        return -1;
    for (i = i >= 0 ? i : 0; i < SNX_HID_TOUCHES && (t->down >> i & 1) && ids[i] != id; ++i) continue;
    if (i == SNX_HID_TOUCHES) return -1;
    t->down |= 1u << i;
    ids[i] = id;
    place(t, i, picture, px, py);
    return i;
}
void touch_move(TouchState* t, const u64* ids, u64 id, const int picture[4], float px, float py) {
    int i = slot_of(t, ids, id);
    if (i >= 0 && picture[2] > 0 && picture[3] > 0) place(t, i, picture, px, py);
}
void touch_up(TouchState* t, const u64* ids, u64 id) {
    int i = slot_of(t, ids, id);
    if (i >= 0) t->down &= ~(1u << i);
}
