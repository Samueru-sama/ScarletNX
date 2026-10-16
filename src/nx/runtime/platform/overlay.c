/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/platform/overlay.h"
#include "runtime/platform/backend.h"
#include "runtime/platform/fatal_screen.h"
#include "runtime/platform/gamepad.h"
#include "runtime/platform/vpad.h"
#include "gui_theme_colors.h"
#include <stdio.h>
#include <string.h>

enum { FIRST = 3, VISIBLE = OVERLAY_ROWS - FIRST - 1 }; /* the items' first row, and how many show at once */
enum { PAGE_MAIN, PAGE_CONTROLS, PAGE_BUTTONS, PAGE_PADS };
enum { M_CONTROLS, M_PADS, M_EXIT, M_COUNT };                           /* the first page's items */
enum { P_LABELS, P_DEADZONE, P_RUMBLE, P_BACKGROUND, P_BACK, P_COUNT }; /* the gamepads' page's */
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
/* the YoRHa menu's colours, from its theme.ini (gui_theme_colors.h), as R G B A bytes; the page lets the game show
   through a little */
#define SNX_OVERLAY_RGBA(rgb, a) ((u32)(a) << 24 | ((rgb) & 0xFFu) << 16 | ((rgb) & 0xFF00u) | ((rgb) >> 16 & 0xFFu))
#define PAGE_RGBA                SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_PAGE, 0xF2u)
#define INK                      SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_INK, 0xFFu)
#define SOFT                     SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_SOFT, 0xFFu)
#define LIGHT                    SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_PAGE, 0xFFu)
#define FAILURE                  SNX_OVERLAY_RGBA(SNX_GUI_THEME_YORHA_FAILURE, 0xFFu)

static int items(void) {
    return page == PAGE_MAIN       ? M_COUNT
           : page == PAGE_CONTROLS ? C_COUNT
           : page == PAGE_PADS     ? P_COUNT
                                   : SNX_VPAD_COUNT + 2;
}
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
        static const char* const main_items[M_COUNT] = {"On-screen controls", "Controllers", "Exit"};
        snprintf(text, size, "%s", main_items[i]);
        if (i != M_EXIT) snprintf(value, vsize, ">");
        return;
    }
    if (page == PAGE_PADS) {
        static const char* const pads[P_COUNT] = {"A B X Y", "Stick dead zone", "Vibration", "Background input",
                                                  "Back"};
        const SnxGamepadConfig* g = gamepad_config();
        snprintf(text, size, "%s", pads[i]);
        if (i == P_LABELS) snprintf(value, vsize, "%s", g->labels ? "By label" : "By position");
        if (i == P_DEADZONE) snprintf(value, vsize, "< %d%% >", g->deadzone);
        if (i == P_RUMBLE && g->rumble) snprintf(value, vsize, "< %d%% >", g->rumble);
        if (i == P_RUMBLE && !g->rumble) snprintf(value, vsize, "< Off >");
        if (i == P_BACKGROUND) snprintf(value, vsize, "%s", g->background ? "On" : "Off");
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
        case C_TOUCH: snprintf(value, vsize, "%s", touch[backend_touch_mode()]); break; /* 0 to 2, as the .ini clamps */
        case C_BUTTONS: snprintf(value, vsize, ">"); break;
        default: break;
    }
}
static void flip(int* v) {
    *v = !*v;
    vpad_store(v);
}
static void flip_pad(int* v) {
    *v = !*v;
    gamepad_store(v);
}
/* the item i done, or its value stepped (step -1 or 1; 0 for a select) */
static int act(int i, int step) {
    SnxVpadConfig* v = vpad_config();
    if (page == PAGE_MAIN) {
        if (step) return OVERLAY_NONE;
        if (i == M_EXIT) return OVERLAY_EXIT;
        page_open(i == M_PADS ? PAGE_PADS : PAGE_CONTROLS);
        return OVERLAY_NONE;
    }
    if (page == PAGE_PADS) {
        SnxGamepadConfig* g = gamepad_config();
        if (i == P_BACK) {
            if (!step) overlay_back();
            return OVERLAY_NONE;
        }
        if (i == P_LABELS || i == P_BACKGROUND) {
            flip_pad(i == P_LABELS ? &g->labels : &g->background);
        } else { /* a select steps up, as a tap on the right arrow */
            int* n = i == P_DEADZONE ? &g->deadzone : &g->rumble;
            *n = clamp(*n + (step ? step : 1) * (i == P_DEADZONE ? 5 : 10), 0, i == P_DEADZONE ? 90 : 100);
            gamepad_store(n);
        }
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
        case C_SHOW: flip(&v->on); break;
        case C_SNAP: flip(&v->snap); break;
        case C_STICK: flip(&v->rel_center); break;
        case C_DPAD: flip(&v->dpad_slide); break;
        case C_HAPTICS: flip(&v->haptics); break;
        case C_SCALE:
        case C_OPACITY: {
            int* n = i == C_SCALE ? &v->scale : &v->opacity;
            *n = clamp(*n + (step ? step : 1) * 10, 0, 100);
            vpad_store(n);
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
static int display_percent = 100; /* set on the event thread, read by the renderer's */
void overlay_set_display_scale(float scale) {
    __atomic_store_n(&display_percent, scale > 0.0f ? (int)(scale * 100.0f) : 100, __ATOMIC_RELAXED);
}
void overlay_place(int sw, int sh, int* x, int* y, int* scale) {
    /* the tallest page sets the scale, so the letters keep their size from page to page: half the picture, or twice
       the display's scale as the menu's Text size Auto (src/gui) if that is more, within three quarters of it. A
       phone's short picture held its letters at 2x by the half alone */
    int w = OVERLAY_COLS * 8, h = OVERLAY_ROWS * 16;
    int s = sw / 2 / w < sh / 2 / h ? sw / 2 / w : sh / 2 / h;
    int dense = 2 * __atomic_load_n(&display_percent, __ATOMIC_RELAXED) / 100;
    if (dense > s) s = dense;
    if (s > sw * 3 / 4 / w) s = sw * 3 / 4 / w;
    if (s > sh * 3 / 4 / h) s = sh * 3 / 4 / h;
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
    int from = page;
    if (page == PAGE_MAIN) return OVERLAY_CLOSE;
    if (page == PAGE_BUTTONS) {
        page_open(PAGE_CONTROLS);
        focus = C_BUTTONS;
    } else {
        page_open(PAGE_MAIN);
        focus = from == PAGE_PADS ? M_PADS : M_CONTROLS;
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
    if ((page == PAGE_CONTROLS && (i == C_SCALE || i == C_OPACITY)) ||
        (page == PAGE_PADS && (i == P_DEADZONE || i == P_RUMBLE)))
        return act(i, col < OVERLAY_COLS - 5 ? -1 : 1);
    return act(i, 0);
}
/* the gamepads' page's second row: the first one connected by name, and how many more */
static const char* pads_hint(char* out, size_t size) {
    int more = 0;
    if (!gamepad_name(0)) return "None connected.";
    while (gamepad_name(more + 1)) ++more;
    if (more)
        snprintf(out, size, "%.27s and %d more", gamepad_name(0), more);
    else
        snprintf(out, size, "%s", gamepad_name(0));
    return out;
}
void overlay_draw(SnxUiList* l, int sw, int sh) {
    static const char* const titles[] = {"SCARLETNX  /  MENU", "SCARLETNX  /  CONTROLS", "SCARLETNX  /  BUTTONS",
                                         "SCARLETNX  /  CONTROLLERS"};
    static const char* const hints[] = {"Back or Home: back to the game.", "Back: the menu.",
                                        "Tap one to show or hide it."};
    char text[OVERLAY_COLS + 1], value[OVERLAY_COLS + 1], pads[OVERLAY_COLS - 1];
    int px, py, s, row, n = items();
    float cw, ch;
    if (!shown) return;
    overlay_place(sw, sh, &px, &py, &s);
    cw = 8.0f * (float)s;
    ch = 16.0f * (float)s;
    ui_fill(l, (float)px, (float)py, cw * OVERLAY_COLS, ch * (float)rows(), PAGE_RGBA);
    ui_fill(l, (float)px, (float)py, cw * OVERLAY_COLS, ch, INK);
    ui_text(l, (float)px + cw, (float)py, s, titles[page], LIGHT);
    ui_text(l, (float)px + cw, (float)py + ch, s, page == PAGE_PADS ? pads_hint(pads, sizeof pads) : hints[page], SOFT);
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
void overlay_error_draw(SnxUiList* l, int sw, int sh, const char* name, const char* msg, const char* how) {
    enum { PARAS = 4, LINES = 12 };
    static char rows_of[PARAS][LINES][FATAL_ROW];
    const char* text[PARAS] = {name, msg, fatal_hint(msg), how};
    const u32 color[PARAS] = {SOFT, FAILURE, INK, SOFT};
    char line[OVERLAY_COLS + 4];
    int px, py, s, n[PARAS], k, i, total, room, row;
    float cw, ch, x, y;
    overlay_place(sw, sh, &px, &py, &s);
    cw = 8.0f * (float)s;
    ch = 16.0f * (float)s;
    for (k = 0; k < PARAS; ++k) n[k] = text[k] ? fatal_wrap(text[k], OVERLAY_COLS - 2, rows_of[k], LINES) : 0;
    /* the head, the name, then each paragraph after a blank row; a blank, Close and a blank. On a short picture the
       last paragraphs lose rows first */
    room = sh / (int)ch;
    for (;;) {
        total = 1 + 4;
        for (k = 0; k < PARAS; ++k) total += (n[k] > LINES ? LINES : n[k]) + (k > 0 && n[k] ? 1 : 0);
        for (k = PARAS - 1; k > 1 && !n[k]; --k) {}
        if (total <= room || k < 1 || !n[k]) break;
        n[k] = (n[k] > LINES ? LINES : n[k]) - 1;
    }
    x = (float)px;
    y = (float)((sh - total * (int)ch) / 2);
    ui_fill(l, x, y, cw * OVERLAY_COLS, ch * (float)total, PAGE_RGBA);
    ui_fill(l, x, y, cw * OVERLAY_COLS, ch, INK);
    ui_text(l, x + cw, y, s, "SCARLETNX  /  STOPPED", LIGHT);
    row = 1;
    for (k = 0; k < PARAS; ++k) {
        if (k > 0 && n[k]) ++row;
        for (i = 0; i < n[k] && i < LINES; ++i, ++row) {
            fatal_cp437(rows_of[k][i], line, sizeof line);
            ui_text(l, x + cw, y + ch * (float)row, s, line, color[k]);
        }
    }
    row = total - 2;
    ui_fill(l, x, y + ch * (float)row, cw * OVERLAY_COLS, ch, INK);
    ui_text(l, x + cw, y + ch * (float)row, s, "\xFE Close", LIGHT);
}
