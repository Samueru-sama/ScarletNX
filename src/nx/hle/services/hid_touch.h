/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The touch screen's state in HID's shared memory, from the fingers the frontend saw: a finger's first sample says it
   started and the one after it lifts says it ended, as the console's driver reports them */
#ifndef SNX_HID_TOUCH_H
#define SNX_HID_TOUCH_H
#include "cpu/aarch64/cpu.h"

#define SNX_HID_TOUCHES      16   /* fingers the touch screen tracks */
#define SNX_HID_TOUCH_WIDTH  1280 /* its size, whatever the screen the game is shown on */
#define SNX_HID_TOUCH_HEIGHT 720

/* the fingers on the screen now, each in its slot for as long as it stays down */
typedef struct {
    u32 down; /* bit i: slot i has a finger */
    u32 x[SNX_HID_TOUCHES], y[SNX_HID_TOUCHES];
} TouchState;
/* what the touch screen keeps of each slot from one sample to the next */
typedef struct {
    u32 pressed, start, end; /* bit masks by slot */
    u32 x[SNX_HID_TOUCHES], y[SNX_HID_TOUCHES];
} HidFingers;

/* the next HidTouchScreenState (HS_HidTouchScreenState bytes at out) */
void hid_touch_sample(HidFingers* fingers, const TouchState* seen, u64 sampling, u8* out);
#endif
