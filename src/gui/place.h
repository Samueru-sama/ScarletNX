/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Where a screen's boxes go when the theme draws it in boxes, in pixels of the area it is drawn in: the drawn edges
   along the top and the bottom, a row of tabs, the title, the list and the box beside it (under it on a narrow screen),
   the status box under that one, in half its column at most, the bar of hints. A column of margin either side and half a row between boxes;
   nothing overlaps. */
#ifndef SNX_GUI_PLACE_H
#define SNX_GUI_PLACE_H

typedef struct {
    int x, y, w, h;
} GuiRect;
typedef struct {
    int width, height;         /* the area */
    int cw, ch;                /* a cell of the text */
    int edge_top, edge_bottom; /* the drawn edges' heights, 0 for none */
    int tabs;                  /* the row of tabs over the title's height, 0 for none */
    int title;                 /* the title's height, 0 for none */
    int bar;                   /* the bar's height, 0 for none */
    int list_cols;             /* the columns the list asks for; 0: one box, the whole width */
    int status;                /* the status box's height, 0 for none */
} GuiPlaceIn;
typedef struct {
    GuiRect edge_top, edge_bottom, tabs, title, list, side, status, bar;
    int stacked; /* the side box under the list: too narrow for both side by side */
} GuiPlace;
#define GUI_PLACE_SIDE_MIN 24 /* the side box's columns at least, beside the list */
void gui_place(const GuiPlaceIn* in, GuiPlace* out);
/* count items item pixels high, pitch apart from the top of a box height high: as many as fit, the first of them
   chosen so focus (-1: none) is among them */
int gui_place_list(int height, int item, int pitch, int count, int focus, int* first);
/* the pitch that spreads count items over height, from least to most */
int gui_place_pitch(int height, int count, int least, int most);
#endif
