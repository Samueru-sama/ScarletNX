/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's touch screen: the frontend's fingers placed on the picture (runtime/platform/touch.c), then the samples
   HID writes from them (hle/services/hid_touch.c), as the console's driver reports a finger's start and end */
#include "runtime/platform/touch.h"
#include "hle/services/hid_layout.h"
#include <assert.h>
#include <string.h>

static u32 field(const u8* state, int entry, int offset) {
    u32 v;
    memcpy(&v, state + HO_HidTouchScreenState_touches + entry * HS_HidTouchState + offset, 4);
    return v;
}
static i32 count(const u8* state) {
    i32 n;
    memcpy(&n, state + HO_HidTouchScreenState_count, 4);
    return n;
}

int main(void) {
    TouchState t;
    HidFingers f;
    u64 ids[SNX_HID_TOUCHES], sampling;
    u8 s[HS_HidTouchScreenState];
    int picture[4], i;
    memset(&t, 0, sizeof t);
    memset(&f, 0, sizeof f);
    /* a phone held wide with the bars shown: the picture is the largest 16:9 in the clear part, centered */
    touch_picture(0, 100, 2400, 1000, picture);
    assert(picture[0] == (2400 - 1777) / 2 && picture[1] == 100 && picture[2] == 1777 && picture[3] == 1000);
    touch_picture(0, 0, 1280, 1024, picture);
    assert(picture[0] == 0 && picture[1] == (1024 - 720) / 2 && picture[2] == 1280 && picture[3] == 720);
    /* on the picture a finger takes a slot and lands in 1280 x 720; off it, nothing */
    touch_picture(0, 0, 2560, 1440, picture);
    assert(touch_down(&t, ids, 7, picture, 1280.0f, 720.0f) == 0 && t.x[0] == 640 && t.y[0] == 360);
    assert(touch_down(&t, ids, 9, picture, 0.0f, 1439.0f) == 1 && t.x[1] == 0 && t.y[1] == 719);
    assert(touch_down(&t, ids, 5, picture, 2560.0f, 10.0f) == -1 && t.down == 3);
    /* the same finger again keeps its slot; a move off the picture stays on its edge */
    assert(touch_down(&t, ids, 7, picture, 1280.0f, 720.0f) == 0 && t.down == 3);
    touch_move(&t, ids, 7, picture, 9000.0f, -50.0f);
    assert(t.x[0] == SNX_HID_TOUCH_WIDTH - 1 && t.y[0] == 0);
    /* the mouse is one more finger, in the first free slot */
    assert(touch_down(&t, ids, SNX_TOUCH_MOUSE, picture, 2.0f, 2.0f) == 2 && t.down == 7);
    touch_up(&t, ids, SNX_TOUCH_MOUSE);
    touch_up(&t, ids, 9);
    assert(t.down == 1);

    /* HID: the first sample has the start, the next none, the one after the lift the end, then nothing */
    memset(&t, 0, sizeof t);
    t.down = 1u << 3;
    t.x[3] = 100;
    t.y[3] = 200;
    sampling = 41;
    hid_touch_sample(&f, &t, sampling, s);
    assert(!memcmp(s, &sampling, 8) && count(s) == 1);
    assert(field(s, 0, HO_HidTouchState_attribute) == 1 && field(s, 0, HO_HidTouchState_finger) == 3);
    assert(field(s, 0, HO_HidTouchState_position_x) == 100 && field(s, 0, HO_HidTouchState_position_y) == 200);
    assert(field(s, 0, HO_HidTouchState_diameter_x) == 90 && field(s, 0, HO_HidTouchState_diameter_y) == 90);
    t.x[3] = 110;
    hid_touch_sample(&f, &t, 42, s);
    assert(count(s) == 1 && field(s, 0, HO_HidTouchState_attribute) == 0);
    hid_touch_sample(&f, &t, 43, s);
    assert(field(s, 0, HO_HidTouchState_position_x) == 110);
    t.down = 0;
    hid_touch_sample(&f, &t, 44, s);
    assert(count(s) == 1 && field(s, 0, HO_HidTouchState_attribute) == 2);
    assert(field(s, 0, HO_HidTouchState_position_x) == 110);
    hid_touch_sample(&f, &t, 45, s);
    assert(count(s) == 0);
    /* two fingers come out in slot order, each with its own id */
    t.down = (1u << 5) | (1u << 1);
    for (i = 0; i < SNX_HID_TOUCHES; ++i) t.x[i] = (u32)i * 10;
    hid_touch_sample(&f, &t, 46, s);
    assert(count(s) == 2 && field(s, 0, HO_HidTouchState_finger) == 1 && field(s, 1, HO_HidTouchState_finger) == 5);
    assert(field(s, 1, HO_HidTouchState_position_x) == 50);
    return 0;
}
