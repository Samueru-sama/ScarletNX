/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/platform/overlay.h"
#include "runtime/platform/vpad.h"
#include <stdio.h>
#include <string.h>

enum { FIRST = 3, VISIBLE = OVERLAY_ROWS - FIRST - 1 }; /* the items' first row, and how many show at once */
enum { PAGE_MAIN, PAGE_CONTROLS, PAGE_BUTTONS };
/* the controls page's items */
enum {
    C_SHOW,
    C_EDIT,
    C_SNAP,
    C_SCALE,
    C_OPACITY,
    C_BUTTONS,
    C_STICK,
    C_DPAD,
    C_HAPTICS,
    C_TOUCH,
    C_RESET,
    C_BACK,
    C_COUNT
};
static const char* const CONTROLS[C_COUNT] = {"Show controls", "Edit layout",  "Snap to grid",          "Scale",
                                              "Opacity",       "Buttons",      "Relative stick center", "D-pad slide",
                                              "Haptics",       "Touch screen", "Reset layout",          "Back"};
static int shown, page, focus, top; /* top: the first item showing */
/* the YoRHa menu's colours, R G B A */
#define PAGE_RGBA 0xF2BED6DAu
#define INK       0xFF3A4145u
#define SOFT      0xFF76868Au
#define LIGHT     0xFFBED6DAu

static int items(void) { return page == PAGE_MAIN ? 2 : page == PAGE_CONTROLS ? C_COUNT : SNX_VPAD_COUNT + 2; }
static void page_open(int p) {
    page = p;
    focus = top = 0;
}
static void keep_in_view(void) {
    if (focus < top) top = focus;
    if (focus >= top + VISIBLE) top = focus - VISIBLE + 1;
}
static int all_shown(void) {
    const SnxVpadConfig* v = vpad_config();
    int c;
    for (c = 0; c < SNX_VPAD_COUNT; ++c)
        if (!v->at[c].shown) return 0;
    return 1;
}
static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* an item's text, and its value at the right */
static void label(int i, char* text, size_t size, char* value, size_t vsize) {
    static const char* const touch[] = {"Off", "Fingers", "Mouse too"};
    const SnxVpadConfig* v = vpad_config();
    *value = 0;
    if (page == PAGE_MAIN) {
        snprintf(text, size, "%s", i ? "Exit" : "On-screen controls");
        if (!i) snprintf(value, vsize, ">");
        return;
    }
    if (page == PAGE_BUTTONS) {
        snprintf(text, size, "%s", i == 0 ? "All" : i == SNX_VPAD_COUNT + 1 ? "Back" : vpad_name(i - 1));
        if (i == 0) snprintf(value, vsize, "%s", all_shown() ? "Shown" : "Some hidden");
        if (i > 0 && i <= SNX_VPAD_COUNT) snprintf(value, vsize, "%s", v->at[i - 1].shown ? "Shown" : "Hidden");
        return;
    }
    snprintf(text, size, "%s", CONTROLS[i]);
    switch (i) {
        case C_SHOW: snprintf(value, vsize, "%s", v->on ? "On" : "Off"); break;
        case C_SNAP: snprintf(value, vsize, "%s", v->snap ? "On" : "Off"); break;
        case C_SCALE: snprintf(value, vsize, "< %d%% >", v->scale + 50); break;
        case C_OPACITY: snprintf(value, vsize, "< %d%% >", v->opacity); break;
        case C_STICK: snprintf(value, vsize, "%s", v->rel_center ? "On" : "Off"); break;
        case C_DPAD: snprintf(value, vsize, "%s", v->dpad_slide ? "On" : "Off"); break;
        case C_HAPTICS: snprintf(value, vsize, "%s", v->haptics ? "On" : "Off"); break;
        case C_TOUCH: snprintf(value, vsize, "%s", touch[clamp(backend_touch_mode(), 0, 2)]); break;
        case C_BUTTONS: snprintf(value, vsize, ">"); break;
        default: break;
    }
}
static void flip(int* v, const char* key) {
    *v = !*v;
    vpad_store(key);
}
/* the item i done, or its value stepped (step -1 or 1; 0 for a select) */
static int act(int i, int step) {
    SnxVpadConfig* v = vpad_config();
    if (page == PAGE_MAIN) {
        if (step) return OVERLAY_NONE;
        if (i == 1) return OVERLAY_EXIT;
        page_open(PAGE_CONTROLS);
        return OVERLAY_NONE;
    }
    if (page == PAGE_BUTTONS) {
        int c, all = all_shown();
        if (i == SNX_VPAD_COUNT + 1) {
            if (!step) overlay_back();
            return OVERLAY_NONE;
        }
        for (c = 0; c < SNX_VPAD_COUNT; ++c)
            if (i == 0 || i - 1 == c) {
                v->at[c].shown = i == 0 ? !all : !v->at[c].shown;
                vpad_store_control(c);
            }
        return OVERLAY_NONE;
    }
    switch (i) {
        case C_SHOW: flip(&v->on, "controls"); break;
        case C_SNAP: flip(&v->snap, "controls_snap"); break;
        case C_STICK: flip(&v->rel_center, "controls_rel_center"); break;
        case C_DPAD: flip(&v->dpad_slide, "controls_dpad_slide"); break;
        case C_HAPTICS: flip(&v->haptics, "controls_haptics"); break;
        case C_SCALE:
        case C_OPACITY: {
            int* n = i == C_SCALE ? &v->scale : &v->opacity;
            *n = clamp(*n + (step ? step : 1) * 10, 0, 100);
            vpad_store(i == C_SCALE ? "controls_scale" : "controls_opacity");
            break;
        }
        case C_TOUCH: backend_touch_set((backend_touch_mode() + (step < 0 ? 2 : 1)) % 3); break;
        case C_EDIT:
            if (!step) return OVERLAY_EDIT;
            break;
        case C_BUTTONS:
            if (!step) page_open(PAGE_BUTTONS);
            break;
        case C_RESET:
            if (!step) vpad_reset_layout();
            break;
        case C_BACK:
            if (!step) page_open(PAGE_MAIN);
            break;
        default: break;
    }
    return OVERLAY_NONE;
}

int overlay_shown(void) { return __atomic_load_n(&shown, __ATOMIC_ACQUIRE); }
void overlay_toggle(void) {
    if (!shown) page_open(PAGE_MAIN);
    __atomic_store_n(&shown, !shown, __ATOMIC_RELEASE);
}
/* the panel's rows: the page's items under the head, a blank row under them */
static int rows(void) { return FIRST + (items() < VISIBLE ? items() : VISIBLE) + 1; }
void overlay_place(int sw, int sh, int* x, int* y, int* scale) {
    /* the tallest page sets the scale, so the letters keep their size from page to page */
    int w = OVERLAY_COLS * 8, h = OVERLAY_ROWS * 16;
    int s = sw / 2 / w < sh / 2 / h ? sw / 2 / w : sh / 2 / h;
    *scale = s < 1 ? 1 : s;
    *x = (sw - w * *scale) / 2;
    *y = (sh - rows() * 16 * *scale) / 2;
}
int overlay_select(void) { return act(focus, 0); }
void overlay_move(int rows) {
    int n = items();
    focus = ((focus + rows) % n + n) % n;
    keep_in_view();
}
void overlay_side(int step) {
    if (page != PAGE_MAIN) act(focus, step);
}
int overlay_back(void) {
    if (page == PAGE_MAIN) return OVERLAY_CLOSE;
    if (page == PAGE_BUTTONS) {
        page_open(PAGE_CONTROLS);
        focus = C_BUTTONS;
    } else {
        page_open(PAGE_MAIN);
    }
    keep_in_view();
    return OVERLAY_NONE;
}
int overlay_tap(float x, float y, int sw, int sh) {
    int px, py, s, row, col, i;
    overlay_place(sw, sh, &px, &py, &s);
    if (x < (float)px || y < (float)py || x >= (float)(px + OVERLAY_COLS * 8 * s) || y >= (float)(py + rows() * 16 * s))
        return OVERLAY_CLOSE;
    row = (int)(y - (float)py) / (16 * s) - FIRST;
    col = (int)(x - (float)px) / (8 * s);
    i = top + row;
    if (row < 0 || row >= VISIBLE || i >= items()) return OVERLAY_NONE;
    focus = i;
    /* a value's arrows: the left half of its last ten columns steps it down */
    if (page == PAGE_CONTROLS && (i == C_SCALE || i == C_OPACITY)) return act(i, col < OVERLAY_COLS - 5 ? -1 : 1);
    return act(i, 0);
}
void overlay_draw(SnxUiList* l, int sw, int sh) {
    static const char* const titles[] = {"SCARLETNX  /  MENU", "SCARLETNX  /  CONTROLS", "SCARLETNX  /  BUTTONS"};
    static const char* const hints[] = {"Back or Home: back to the game.", "Back: the menu.",
                                        "Tap one to show or hide it."};
    char text[OVERLAY_COLS + 1], value[OVERLAY_COLS + 1];
    int px, py, s, row, n = items();
    float cw, ch;
    if (!shown) return;
    overlay_place(sw, sh, &px, &py, &s);
    cw = 8.0f * (float)s;
    ch = 16.0f * (float)s;
    ui_fill(l, (float)px, (float)py, cw * OVERLAY_COLS, ch * (float)rows(), PAGE_RGBA);
    ui_fill(l, (float)px, (float)py, cw * OVERLAY_COLS, ch, INK);
    ui_text(l, (float)px + cw, (float)py, s, titles[page], LIGHT);
    ui_text(l, (float)px + cw, (float)py + ch, s, hints[page], SOFT);
    for (row = 0; row < VISIBLE && top + row < n; ++row) {
        int i = top + row, on = i == focus;
        float y = (float)py + ch * (float)(FIRST + row);
        char line[OVERLAY_COLS + 4];
        label(i, text, sizeof text, value, sizeof value);
        if (on) ui_fill(l, (float)px, y, cw * OVERLAY_COLS, ch, INK);
        snprintf(line, sizeof line, "\xFE %s", text);
        ui_text(l, (float)px + cw, y, s, line, on ? LIGHT : INK);
        ui_text(l, (float)px + cw * (float)(OVERLAY_COLS - 1 - (int)strlen(value)), y, s, value, on ? LIGHT : INK);
    }
    /* more above or under: the font's arrows at the edge */
    if (top) ui_text(l, (float)px + cw * (OVERLAY_COLS - 1), (float)py + ch * FIRST, s, "\x18", SOFT);
    if (top + VISIBLE < n)
        ui_text(l, (float)px + cw * (OVERLAY_COLS - 1), (float)py + ch * (FIRST + VISIBLE - 1), s, "\x19", SOFT);
}
