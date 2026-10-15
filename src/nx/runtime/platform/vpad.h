/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls: a pad drawn over the game (ui.h) that fingers or the mouse press, with rEden's touch
   overlay's sprites, sizes and options, laid out as the console's two controllers hold them. The same on every
   platform: on by default on Android, off elsewhere. Kept in the game's .ini (keys controls and controls_*) */
#ifndef SNX_VPAD_H
#define SNX_VPAD_H
#include "runtime/platform/ui.h"

/* in the order they are drawn: a finger presses the last one drawn under it */
enum {
    SNX_VPAD_L,
    SNX_VPAD_R,
    SNX_VPAD_ZL,
    SNX_VPAD_ZR,
    SNX_VPAD_MINUS,
    SNX_VPAD_PLUS,
    SNX_VPAD_CAPTURE,
    SNX_VPAD_HOME,
    SNX_VPAD_L3,
    SNX_VPAD_R3,
    SNX_VPAD_DPAD,
    SNX_VPAD_STICK_L,
    SNX_VPAD_STICK_R,
    SNX_VPAD_A,
    SNX_VPAD_B,
    SNX_VPAD_X,
    SNX_VPAD_Y,
    SNX_VPAD_COUNT
};
/* what the frontend does, read with vpad_actions */
#define SNX_VPAD_DO_MENU 1u /* Home was pressed: the game's menu */
#define SNX_VPAD_DO_SHOT 2u /* Capture was pressed: a screenshot */
#define SNX_VPAD_DO_BUZZ 4u /* a control went down or up: a short buzz, haptics being on */

typedef struct {
    float x, y; /* its middle, 0 to 1 across the window's clear part */
    int size;   /* percent of its own default size */
    int shown;
} SnxVpadPlace;
typedef struct {
    int on;         /* drawn, taking the fingers that land on it */
    int scale;      /* rEden's 0 to 100: every control at (scale + 50)% */
    int opacity;    /* percent */
    int snap;       /* a control dragged in edit mode lands on the grid */
    int rel_center; /* a stick's middle where its finger lands */
    int dpad_slide; /* a finger sliding on the D-pad changes its direction */
    int haptics;    /* a buzz on each press and release */
    SnxVpadPlace at[SNX_VPAD_COUNT];
} SnxVpadConfig;
/* the settings, their defaults filled in on first use */
SnxVpadConfig* vpad_config(void);
#define SNX_VPAD_SIZE_MIN 50
#define SNX_VPAD_SIZE_MAX 200

const char* vpad_name(int control); /* "A", "Left stick"... */
void vpad_defaults(SnxVpadConfig* c);
/* places, sizes and the shown ones back to their defaults, kept */
void vpad_reset_layout(void);
/* a key of the game's .ini (controls, controls_scale..., controls_a...): applied, 1; not one of these, 0 */
int vpad_ini(const char* key, const char* value);
/* where changed settings go: called with each key and its value (the game's .ini) */
void vpad_set_store(void (*store)(const char* key, const char* value));
/* a setting's current value to the store: "controls", "controls_scale"..., or a control's ("controls_a") */
void vpad_store(const char* key);
void vpad_store_control(int control);

/* Fingers, and the mouse as one, in window pixels; safe is the window's clear part (x, y, w, h). A finger that lands
   on a control, or any in edit mode, is the controls': 1, and its moves and lift are theirs */
int vpad_down(u64 id, float x, float y, const int safe[4]);
void vpad_move(u64 id, float x, float y, const int safe[4]);
int vpad_up(u64 id);
void vpad_release(void); /* every finger off: the menu opened, the window went away */
/* the controls' buttons as HidNpadButton bits, and their sticks (lx, ly, rx, ry) from -32767 to 32767, right and up */
void vpad_state(u64* buttons, i32 sticks[4]);
unsigned vpad_actions(void); /* SNX_VPAD_DO_ bits since the last call */

/* edit mode: a control dragged moves, one tapped can be sized from the bar on top, Done (or vpad_edit(0)) ends it */
void vpad_edit(int on);
int vpad_editing(void);
int vpad_shown(void); /* drawn: on, or in edit mode */

/* safe in the pixels of the picture drawn on */
void vpad_draw(SnxUiList* l, const int safe[4]);
#endif
