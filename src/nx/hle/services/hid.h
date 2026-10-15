/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* input (hid.c) */
#ifndef SNX_HID_H
#define SNX_HID_H
#include "cpu/aarch64/cpu.h"
#include "hle/services/hid_touch.h"

typedef struct {
    u64 buttons;
    i32 lx, ly, rx, ry;
} PadState;
extern PadState g_pad;
extern TouchState g_touch; /* the frontend's fingers, which hid_update turns into the touch screen's samples */
void hid_update(void);

#endif
