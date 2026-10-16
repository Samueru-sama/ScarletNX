/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu: Android's back button, a gamepad's Home, the on-screen Home or F2 opens it over the game, in the
   menu's look (src/gui, the YoRHa theme), drawn by the renderer as quads (ui.h). The same on every platform: the
   on-screen controls' options (vpad.h), the touch screen's, and Exit */
#ifndef SNX_OVERLAY_H
#define SNX_OVERLAY_H
#include "runtime/platform/ui.h"

#define OVERLAY_COLS 40 /* the panel, in cells of the 8x16 font */
#define OVERLAY_ROWS 16 /* the tallest page's: the others are as tall as their items */
enum { OVERLAY_NONE, OVERLAY_CLOSE, OVERLAY_EXIT, OVERLAY_EDIT };

/* whether it is open, for the renderer's thread too */
int overlay_shown(void);
void overlay_toggle(void);
/* the display's own scale (SDL's: dpi / 160 on a phone), which the panel's letters follow; 1 until told */
void overlay_set_display_scale(float scale);
/* where the panel goes on a picture of sw x sh pixels: its corner and the whole number it is scaled by */
void overlay_place(int sw, int sh, int* x, int* y, int* scale);
int overlay_select(void);      /* the focused item's action: a page, a setting changed, or an OVERLAY_ for the caller */
void overlay_move(int rows);   /* the focus up (-1) or down (1), round the page */
void overlay_scroll(int rows); /* the mouse's wheel: the same, stopping at the ends */
void overlay_side(int step);   /* the focused setting down (-1) or up (1) */
int overlay_back(void);        /* the page above; OVERLAY_CLOSE from the first */
/* a tap at x, y on a picture of sw x sh: what it did, OVERLAY_CLOSE outside the panel */
int overlay_tap(float x, float y, int sw, int sh);
void overlay_draw(SnxUiList* l, int sw, int sh);
/* a fatal error's panel (backend/sdl3.c), the menu's own: the head, the game's name, the error, what it means and what to
   do, cut to its width, and Close in focus */
void overlay_error_draw(SnxUiList* l, int sw, int sh, const char* name, const char* msg, const char* how);

#endif
