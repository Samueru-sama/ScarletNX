/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The controllers SDL knows as gamepads (backend/gamepad.c): each one drives the game's one controller, as the
   keyboard does */
#ifndef SNX_GAMEPAD_H
#define SNX_GAMEPAD_H
#include "cpu/aarch64/cpu.h"
#include "runtime/ini.h"

#define SNX_GAMEPADS 4 /* read at once; one more takes the place of one that goes */
/* a gamepad the system added or removed (its SDL joystick id): opened, closed */
void gamepad_open(u32 id);
void gamepad_close(u32 id);
/* the open gamepads together: their HidNpadButton bits, and the first stick past the dead zone of each side (lx, ly,
   rx, ry; right and up) */
void gamepad_state(u64* buttons, i32 sticks[4]);
/* the HidNpadButton bit one of a gamepad's buttons (an SDL_GamepadButton) is, as the game reads it; 0 for none */
u64 gamepad_button(u32 id, int button);
/* the vibration the game sent its left and right devices (hid.h's PadState.rumble), played on every gamepad that can:
   called each frame */
void gamepad_rumble(u32 left, u32 right);
/* the settings, kept in the game's .ini; the game's menu changes them under ui_lock */
typedef struct {
    int labels;     /* A, B, X and Y: 1 as their labels say, 0 where they sit on the console's controller */
    int deadzone;   /* the sticks', in percent of their travel */
    int rumble;     /* the vibration's strength in percent, 0 for none */
    int background; /* 1: they play while the game's window is not focused */
} SnxGamepadConfig;
SnxGamepadConfig* gamepad_config(void);
void gamepad_store(const int* setting); /* one of gamepad_config()'s, to the .ini, and applied */
/* the open gamepads' names, from 0 on: NULL past the last. Under ui_lock */
const char* gamepad_name(int i);
extern const IniOpt g_gamepad_ini[];                 /* the settings, keys gamepad_* */
int gamepad_ini(const char* key, const char* value); /* a key of the game's .ini: applied, 1; not one of these, 0 */

#endif
