/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "hle/services/hid_touch.h"
#include "hle/services/hid_layout.h"
#include <string.h>

#define SNX_HID_TOUCH_STARTED  1u
#define SNX_HID_TOUCH_ENDED    2u
#define SNX_HID_TOUCH_DIAMETER 90 /* a finger's, in the screen's pixels, as Eden reports it */

void hid_touch_sample(HidFingers* f, const TouchState* seen, u64 sampling, u8* out) {
    u32 i, bit, attribute, diameter = SNX_HID_TOUCH_DIAMETER;
    i32 count = 0;
    u8* t;
    for (i = 0; i < SNX_HID_TOUCHES; ++i) {
        bit = 1u << i;
        /* a start or an end lasts one sample: the one after a start keeps the place, the one after an end drops it */
        if (f->start & bit) {
            f->start &= ~bit;
            continue;
        }
        if (f->end & bit) {
            f->end &= ~bit;
            f->pressed &= ~bit;
            continue;
        }
        if (!(f->pressed & bit) && (seen->down & bit)) {
            f->pressed |= bit;
            f->start |= bit;
        } else if ((f->pressed & bit) && !(seen->down & bit)) {
            f->end |= bit; /* at the place it lifted from */
            continue;
        }
        if (seen->down & bit) {
            f->x[i] = seen->x[i];
            f->y[i] = seen->y[i];
        }
    }
    memset(out, 0, HS_HidTouchScreenState);
    memcpy(out, &sampling, 8);
    for (i = 0; i < SNX_HID_TOUCHES; ++i) {
        bit = 1u << i;
        if (!(f->pressed & bit)) continue;
        t = out + HO_HidTouchScreenState_touches + (u32)count * HS_HidTouchState;
        attribute = (f->start & bit ? SNX_HID_TOUCH_STARTED : 0) | (f->end & bit ? SNX_HID_TOUCH_ENDED : 0);
        memcpy(t + HO_HidTouchState_attribute, &attribute, 4);
        memcpy(t + HO_HidTouchState_finger, &i, 4);
        memcpy(t + HO_HidTouchState_position_x, &f->x[i], 4);
        memcpy(t + HO_HidTouchState_position_y, &f->y[i], 4);
        memcpy(t + HO_HidTouchState_diameter_x, &diameter, 4);
        memcpy(t + HO_HidTouchState_diameter_y, &diameter, 4);
        ++count;
    }
    memcpy(out + HO_HidTouchScreenState_count, &count, 4);
}
