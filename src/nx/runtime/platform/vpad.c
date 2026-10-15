/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/platform/vpad.h"
#include "runtime/platform/controls_atlas.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KIND_BUTTON, KIND_DPAD, KIND_STICK };
typedef struct {
    const char *key, *name;
    int sprite, kind;
    int bit;    /* its HidNpadButton bit, -1 for none */
    float size; /* its longer side, of the clear part's shorter one, as rEden sizes it */
    float x, y; /* where it starts: the console's controllers, the left one's on the left */
} Info;
static const Info INFO[SNX_VPAD_COUNT] = {
    {"l", "L", SNX_SPRITE_L, KIND_BUTTON, 6, 0.26f, 0.09f, 0.21f},
    {"r", "R", SNX_SPRITE_R, KIND_BUTTON, 7, 0.26f, 0.91f, 0.21f},
    {"zl", "ZL", SNX_SPRITE_ZL, KIND_BUTTON, 8, 0.26f, 0.09f, 0.08f},
    {"zr", "ZR", SNX_SPRITE_ZR, KIND_BUTTON, 9, 0.26f, 0.91f, 0.08f},
    {"minus", "-", SNX_SPRITE_MINUS, KIND_BUTTON, 11, 0.07f, 0.25f, 0.07f},
    {"plus", "+", SNX_SPRITE_PLUS, KIND_BUTTON, 10, 0.07f, 0.75f, 0.07f},
    {"capture", "Capture", SNX_SPRITE_CAPTURE, KIND_BUTTON, -1, 0.07f, 0.36f, 0.90f},
    {"home", "Home", SNX_SPRITE_HOME, KIND_BUTTON, -1, 0.07f, 0.64f, 0.90f},
    {"l3", "L3", SNX_SPRITE_L3, KIND_BUTTON, 4, 0.155f, 0.25f, 0.36f},
    {"r3", "R3", SNX_SPRITE_R3, KIND_BUTTON, 5, 0.155f, 0.64f, 0.62f},
    {"dpad", "D-pad", SNX_SPRITE_DPAD, KIND_DPAD, -1, 0.25f, 0.23f, 0.78f},
    {"stick_l", "Left stick", SNX_SPRITE_STICK_RANGE, KIND_STICK, -1, 0.30f, 0.12f, 0.47f},
    {"stick_r", "Right stick", SNX_SPRITE_STICK_RANGE, KIND_STICK, -1, 0.30f, 0.77f, 0.78f},
    {"a", "A", SNX_SPRITE_A, KIND_BUTTON, 0, 0.11f, 0.93f, 0.47f},
    {"b", "B", SNX_SPRITE_B, KIND_BUTTON, 1, 0.11f, 0.87f, 0.60f},
    {"x", "X", SNX_SPRITE_X, KIND_BUTTON, 2, 0.11f, 0.87f, 0.34f},
    {"y", "Y", SNX_SPRITE_Y, KIND_BUTTON, 3, 0.11f, 0.81f, 0.47f},
};
/* the D-pad's directions (HidNpadButton bits 12 to 15) */
#define DPAD_LEFT  (1u << 12)
#define DPAD_UP    (1u << 13)
#define DPAD_RIGHT (1u << 14)
#define DPAD_DOWN  (1u << 15)
#define FINGERS    10
#define STICK_KNOB 1.66f /* the ring is this many knobs wide (rEden) */
#define DEADZONE   0.5f  /* of the D-pad's half: rEden's */

#ifdef __ANDROID__
#define DEFAULT_ON 1
#else
#define DEFAULT_ON 0
#endif

static SnxVpadConfig g_vpad = {DEFAULT_ON, 50, 100, 0, 1, 1, 1, {{0}}};
static int g_placed; /* g_vpad.at has its defaults: set on first use, a static initializer cannot read INFO */
static void (*g_store)(const char* key, const char* value);
typedef struct {
    u64 id;
    int control;  /* -1: a finger of edit mode on no control */
    float cx, cy; /* a stick's middle while it is held; in edit mode, the finger's offset from the control's */
    int moved;
} Finger;
static Finger g_fingers[FINGERS];
static int g_nfingers;
static u32 g_held;           /* a bit per control held down */
static u32 g_dpad;           /* DPAD_ bits */
static float g_axis[2][2];   /* each stick, -1 to 1 right and down */
static float g_middle[2][2]; /* each stick's middle while held, 0 to 1 across the clear part as places are */
static unsigned g_actions;
static int g_edit, g_selected = -1;

static void placed(void) {
    if (!g_placed) {
        int c;
        g_placed = 1;
        for (c = 0; c < SNX_VPAD_COUNT; ++c) {
            g_vpad.at[c].x = INFO[c].x;
            g_vpad.at[c].y = INFO[c].y;
            g_vpad.at[c].size = 100;
            g_vpad.at[c].shown = 1;
        }
    }
}
SnxVpadConfig* vpad_config(void) {
    placed();
    return &g_vpad;
}
const char* vpad_name(int control) { return control >= 0 && control < SNX_VPAD_COUNT ? INFO[control].name : ""; }
void vpad_defaults(SnxVpadConfig* c) {
    int i;
    placed();
    c->on = DEFAULT_ON;
    c->scale = 50;
    c->opacity = 100;
    c->snap = 0;
    c->rel_center = c->dpad_slide = c->haptics = 1;
    for (i = 0; i < SNX_VPAD_COUNT; ++i) {
        c->at[i].x = INFO[i].x;
        c->at[i].y = INFO[i].y;
        c->at[i].size = 100;
        c->at[i].shown = 1;
    }
}
void vpad_reset_layout(void) {
    SnxVpadConfig d;
    int c;
    vpad_defaults(&d);
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        g_vpad.at[c] = d.at[c];
        vpad_store_control(c);
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the settings that are a number, by their key */
static int* number(const char* key, int* lo, int* hi) {
    *lo = 0;
    *hi = 1;
    if (!strcmp(key, "controls")) return &g_vpad.on;
    if (!strcmp(key, "controls_snap")) return &g_vpad.snap;
    if (!strcmp(key, "controls_rel_center")) return &g_vpad.rel_center;
    if (!strcmp(key, "controls_dpad_slide")) return &g_vpad.dpad_slide;
    if (!strcmp(key, "controls_haptics")) return &g_vpad.haptics;
    *hi = 100;
    if (!strcmp(key, "controls_scale")) return &g_vpad.scale;
    if (!strcmp(key, "controls_opacity")) return &g_vpad.opacity;
    return NULL;
}
static int control_of(const char* key) {
    int c;
    if (strncmp(key, "controls_", 9)) return -1;
    for (c = 0; c < SNX_VPAD_COUNT; ++c)
        if (!strcmp(key + 9, INFO[c].key)) return c;
    return -1;
}
int vpad_ini(const char* key, const char* value) {
    int lo, hi, c, *n = number(key, &lo, &hi);
    placed();
    if (n) {
        *n = clampi(atoi(value), lo, hi);
        if (n == &g_vpad.on && !g_vpad.on) vpad_release();
        return 1;
    }
    c = control_of(key);
    if (c >= 0) { /* x y size shown */
        SnxVpadPlace p = g_vpad.at[c];
        if (sscanf(value, "%f %f %d %d", &p.x, &p.y, &p.size, &p.shown) == 4) {
            p.x = clampf(p.x, 0, 1);
            p.y = clampf(p.y, 0, 1);
            p.size = clampi(p.size, SNX_VPAD_SIZE_MIN, SNX_VPAD_SIZE_MAX);
            p.shown = !!p.shown;
            g_vpad.at[c] = p;
        }
        return 1;
    }
    return 0;
}
void vpad_set_store(void (*store)(const char* key, const char* value)) { g_store = store; }
void vpad_store(const char* key) {
    char v[64];
    int lo, hi, c, *n = number(key, &lo, &hi);
    if (n)
        snprintf(v, sizeof v, "%d", *n);
    else if ((c = control_of(key)) >= 0)
        snprintf(v, sizeof v, "%.3f %.3f %d %d", g_vpad.at[c].x, g_vpad.at[c].y, g_vpad.at[c].size, g_vpad.at[c].shown);
    else
        return;
    if (g_store) g_store(key, v);
}
void vpad_store_control(int control) {
    char key[32];
    if (control < 0 || control >= SNX_VPAD_COUNT) return;
    snprintf(key, sizeof key, "controls_%s", INFO[control].key);
    vpad_store(key);
}

/* a control's rectangle (x, y, w, h) on a clear part of safe */
static void box(int c, const int safe[4], float r[4]) {
    const unsigned short* s = snx_controls_sprites[INFO[c].sprite];
    float shorter = (float)(safe[2] < safe[3] ? safe[2] : safe[3]), big = (float)(s[2] > s[3] ? s[2] : s[3]);
    float side = INFO[c].size * shorter * (float)(g_vpad.scale + 50) / 100 * (float)g_vpad.at[c].size / 100;
    r[2] = side * (float)s[2] / big;
    r[3] = side * (float)s[3] / big;
    r[0] = (float)safe[0] + g_vpad.at[c].x * (float)safe[2] - r[2] / 2;
    r[1] = (float)safe[1] + g_vpad.at[c].y * (float)safe[3] - r[3] / 2;
}
static int inside(const float r[4], float x, float y) {
    return x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3];
}
static int hit(float x, float y, const int safe[4]) {
    int c;
    float r[4];
    for (c = SNX_VPAD_COUNT - 1; c >= 0; --c) {
        box(c, safe, r);
        if (g_vpad.at[c].shown && inside(r, x, y)) return c;
    }
    return -1;
}
static Finger* finger(u64 id) {
    int i;
    for (i = 0; i < g_nfingers; ++i)
        if (g_fingers[i].id == id) return &g_fingers[i];
    return NULL;
}
static int stick_of(int c) { return c == SNX_VPAD_STICK_R; }
static void buzz(void) {
    if (g_vpad.haptics) g_actions |= SNX_VPAD_DO_BUZZ;
}
static void dpad_at(int c, float x, float y, const int safe[4]) {
    float r[4], ax, ay;
    u32 was = g_dpad;
    box(c, safe, r);
    ax = (x - r[0] - r[2] / 2) / (r[2] / 2);
    ay = (y - r[1] - r[3] / 2) / (r[3] / 2);
    g_dpad = (ax < -DEADZONE ? DPAD_LEFT : 0) | (ax > DEADZONE ? DPAD_RIGHT : 0) | (ay < -DEADZONE ? DPAD_UP : 0) |
             (ay > DEADZONE ? DPAD_DOWN : 0);
    if (g_dpad != was) buzz();
}
static void stick_at(int c, float x, float y, const int safe[4]) {
    float r[4], ax, ay, len;
    int s = stick_of(c);
    box(c, safe, r);
    ax = (x - (float)safe[0] - g_middle[s][0] * (float)safe[2]) / (r[2] / 2);
    ay = (y - (float)safe[1] - g_middle[s][1] * (float)safe[3]) / (r[3] / 2);
    len = sqrtf(ax * ax + ay * ay);
    if (len > 1) {
        ax /= len;
        ay /= len;
    }
    g_axis[s][0] = ax;
    g_axis[s][1] = ay;
}

/* edit mode's bar on top: less, the selected control's size, more, done; in cells of 8x16 at a whole scale */
enum { BAR_LESS, BAR_SIZE, BAR_MORE, BAR_DONE, BAR_COUNT };
static const int BAR_COLS[BAR_COUNT] = {5, 18, 5, 8};
static int bar_scale(const int safe[4]) {
    int s = (safe[2] < safe[3] ? safe[2] : safe[3]) / 360;
    return s < 1 ? 1 : s;
}
static void bar(const int safe[4], float r[BAR_COUNT][4]) {
    int i, s = bar_scale(safe), total = 0;
    float x;
    for (i = 0; i < BAR_COUNT; ++i) total += BAR_COLS[i] + (i ? 2 : 0);
    x = (float)safe[0] + ((float)safe[2] - (float)(total * 8 * s)) / 2;
    for (i = 0; i < BAR_COUNT; ++i) {
        r[i][0] = x;
        r[i][1] = (float)(safe[1] + 8 * s);
        r[i][2] = (float)(BAR_COLS[i] * 8 * s);
        r[i][3] = (float)(16 * s);
        x += (float)((BAR_COLS[i] + 2) * 8 * s);
    }
}
static float grid(const int safe[4]) { return (float)(safe[2] < safe[3] ? safe[2] : safe[3]) / 32; }
static void resize(int step) {
    if (g_selected < 0) return;
    g_vpad.at[g_selected].size = clampi(g_vpad.at[g_selected].size + step, SNX_VPAD_SIZE_MIN, SNX_VPAD_SIZE_MAX);
    vpad_store_control(g_selected);
}
static int edit_down(Finger* f, float x, float y, const int safe[4]) {
    float b[BAR_COUNT][4], r[4];
    bar(safe, b);
    f->control = -1;
    if (inside(b[BAR_DONE], x, y)) {
        vpad_edit(0);
        return 1;
    }
    if (inside(b[BAR_LESS], x, y) || inside(b[BAR_MORE], x, y)) {
        resize(inside(b[BAR_LESS], x, y) ? -10 : 10);
        return 1;
    }
    f->control = hit(x, y, safe);
    g_selected = f->control;
    if (f->control >= 0) {
        box(f->control, safe, r);
        f->cx = r[0] + r[2] / 2 - x;
        f->cy = r[1] + r[3] / 2 - y;
    }
    return 1;
}
static void edit_move(Finger* f, float x, float y, const int safe[4]) {
    float cx = x + f->cx, cy = y + f->cy, g = grid(safe);
    if (f->control < 0 || safe[2] <= 0 || safe[3] <= 0) return;
    if (g_vpad.snap && g > 0) {
        cx = (float)safe[0] + floorf((cx - (float)safe[0]) / g + 0.5f) * g;
        cy = (float)safe[1] + floorf((cy - (float)safe[1]) / g + 0.5f) * g;
    }
    g_vpad.at[f->control].x = clampf((cx - (float)safe[0]) / (float)safe[2], 0, 1);
    g_vpad.at[f->control].y = clampf((cy - (float)safe[1]) / (float)safe[3], 0, 1);
    f->moved = 1;
}

int vpad_down(u64 id, float x, float y, const int safe[4]) {
    Finger* f;
    int c;
    placed();
    if ((!g_vpad.on && !g_edit) || finger(id) || g_nfingers == FINGERS) return 0;
    c = g_edit ? -1 : hit(x, y, safe);
    if (!g_edit && c < 0) return 0;
    f = &g_fingers[g_nfingers++];
    memset(f, 0, sizeof *f);
    f->id = id;
    f->control = c;
    if (g_edit) return edit_down(f, x, y, safe);
    if (INFO[c].kind == KIND_DPAD) {
        dpad_at(c, x, y, safe);
        if (!g_dpad) buzz(); /* its middle: held, no direction yet */
    } else if (INFO[c].kind == KIND_STICK) {
        int s = stick_of(c);
        g_middle[s][0] = g_vpad.rel_center ? (x - (float)safe[0]) / (float)safe[2] : g_vpad.at[c].x;
        g_middle[s][1] = g_vpad.rel_center ? (y - (float)safe[1]) / (float)safe[3] : g_vpad.at[c].y;
        stick_at(c, x, y, safe);
        buzz();
    } else {
        if (c == SNX_VPAD_HOME) g_actions |= SNX_VPAD_DO_MENU;
        if (c == SNX_VPAD_CAPTURE) g_actions |= SNX_VPAD_DO_SHOT;
        buzz();
    }
    g_held |= 1u << c;
    return 1;
}
void vpad_move(u64 id, float x, float y, const int safe[4]) {
    Finger* f = finger(id);
    if (!f) return;
    if (g_edit)
        edit_move(f, x, y, safe);
    else if (INFO[f->control].kind == KIND_DPAD && g_vpad.dpad_slide)
        dpad_at(f->control, x, y, safe);
    else if (INFO[f->control].kind == KIND_STICK)
        stick_at(f->control, x, y, safe);
}
static void lift(Finger* f) {
    int c = f->control, i;
    *f = g_fingers[--g_nfingers];
    if (g_edit || c < 0) return;
    for (i = 0; i < g_nfingers; ++i)
        if (g_fingers[i].control == c) return; /* another finger still holds it */
    g_held &= ~(1u << c);
    if (INFO[c].kind == KIND_DPAD) g_dpad = 0;
    if (INFO[c].kind == KIND_STICK) g_axis[stick_of(c)][0] = g_axis[stick_of(c)][1] = 0;
    buzz();
}
int vpad_up(u64 id) {
    Finger* f = finger(id);
    if (!f) return 0;
    if (g_edit && f->control >= 0 && f->moved) vpad_store_control(f->control);
    lift(f);
    return 1;
}
void vpad_release(void) {
    while (g_nfingers) lift(&g_fingers[0]);
    g_held = g_dpad = 0;
    memset(g_axis, 0, sizeof g_axis);
}
void vpad_state(u64* buttons, i32 sticks[4]) {
    int c, s;
    *buttons = g_dpad;
    for (c = 0; c < SNX_VPAD_COUNT; ++c)
        if ((g_held >> c & 1) && INFO[c].bit >= 0) *buttons |= 1ull << INFO[c].bit;
    for (s = 0; s < 2; ++s) {
        sticks[s * 2] = (i32)(g_axis[s][0] * 32767);
        sticks[s * 2 + 1] = (i32)(-g_axis[s][1] * 32767);
    }
}
unsigned vpad_actions(void) {
    unsigned a = g_actions;
    g_actions = 0;
    return a;
}
void vpad_edit(int on) {
    vpad_release();
    g_edit = on;
    g_selected = -1;
}
int vpad_editing(void) { return g_edit; }
int vpad_shown(void) { return g_vpad.on || g_edit; }

/* the pressed D-pad: one direction is the up sprite turned, two the up-left one */
static void dpad_draw(SnxUiList* l, const float r[4], u32 alpha) {
    static const u32 one[4] = {DPAD_UP, DPAD_RIGHT, DPAD_DOWN, DPAD_LEFT};
    static const u32 two[4] = {DPAD_UP | DPAD_LEFT, DPAD_UP | DPAD_RIGHT, DPAD_DOWN | DPAD_RIGHT,
                               DPAD_DOWN | DPAD_LEFT};
    int t;
    for (t = 0; t < 4; ++t) {
        if (g_dpad == one[t]) {
            ui_sprite(l, SNX_SPRITE_DPAD_UP, r[0], r[1], r[2], r[3], alpha, t);
            return;
        }
        if (g_dpad == two[t]) {
            ui_sprite(l, SNX_SPRITE_DPAD_UP_LEFT, r[0], r[1], r[2], r[3], alpha, t);
            return;
        }
    }
    ui_sprite(l, SNX_SPRITE_DPAD, r[0], r[1], r[2], r[3], alpha, 0);
}
static void stick_draw(SnxUiList* l, int c, float r[4], const int safe[4], u32 alpha) {
    int s = stick_of(c), held = g_held >> c & 1;
    float kw = r[2] / STICK_KNOB, kh = r[3] / STICK_KNOB, mx = r[0] + r[2] / 2, my = r[1] + r[3] / 2;
    if (held && !g_edit) { /* the ring where the finger landed, the knob where it is */
        mx = (float)safe[0] + g_middle[s][0] * (float)safe[2];
        my = (float)safe[1] + g_middle[s][1] * (float)safe[3];
        r[0] = mx - r[2] / 2;
        r[1] = my - r[3] / 2;
        mx += g_axis[s][0] * r[2] / 2;
        my += g_axis[s][1] * r[3] / 2;
    }
    ui_sprite(l, SNX_SPRITE_STICK_RANGE, r[0], r[1], r[2], r[3], alpha, 0);
    ui_sprite(l, held && !g_edit ? SNX_SPRITE_STICK_PRESSED : SNX_SPRITE_STICK, mx - kw / 2, my - kh / 2, kw, kh, alpha,
              0);
}
static void outline(SnxUiList* l, const float r[4], float t, u32 rgba) {
    ui_fill(l, r[0] - t, r[1] - t, r[2] + 2 * t, t, rgba);
    ui_fill(l, r[0] - t, r[1] + r[3], r[2] + 2 * t, t, rgba);
    ui_fill(l, r[0] - t, r[1], t, r[3], rgba);
    ui_fill(l, r[0] + r[2], r[1], t, r[3], rgba);
}
static void edit_draw(SnxUiList* l, const int safe[4]) {
    const u32 ink = 0xF03A4145u, page = 0xFFBED6DAu, line = 0x40FFFFFFu;
    float b[BAR_COUNT][4], r[4], g = grid(safe), v;
    int s = bar_scale(safe);
    char label[32];
    if (g_vpad.snap && g > 0) {
        for (v = (float)safe[0]; v <= (float)(safe[0] + safe[2]); v += g)
            ui_fill(l, v, (float)safe[1], 1, (float)safe[3], line);
        for (v = (float)safe[1]; v <= (float)(safe[1] + safe[3]); v += g)
            ui_fill(l, (float)safe[0], v, (float)safe[2], 1, line);
    }
    if (g_selected >= 0) {
        box(g_selected, safe, r);
        outline(l, r, (float)(2 * s), 0xFF00C8FFu);
    }
    bar(safe, b);
    ui_fill(l, b[0][0] - (float)(8 * s), b[0][1] - (float)(4 * s),
            b[BAR_DONE][0] + b[BAR_DONE][2] - b[0][0] + (float)(16 * s), (float)(24 * s), ink);
    ui_text(l, b[BAR_LESS][0], b[BAR_LESS][1], s, "[ - ]", page);
    if (g_selected >= 0)
        snprintf(label, sizeof label, "%s %d%%", INFO[g_selected].name, g_vpad.at[g_selected].size);
    else
        snprintf(label, sizeof label, "Tap a control");
    ui_text(l, b[BAR_SIZE][0] + (b[BAR_SIZE][2] - (float)(strlen(label) * 8 * (size_t)s)) / 2, b[BAR_SIZE][1], s, label,
            page);
    ui_text(l, b[BAR_MORE][0], b[BAR_MORE][1], s, "[ + ]", page);
    ui_text(l, b[BAR_DONE][0], b[BAR_DONE][1], s, "[ Done ]", page);
}
void vpad_draw(SnxUiList* l, const int safe[4]) {
    int c;
    float r[4];
    u32 alpha;
    placed();
    if (!g_vpad.on && !g_edit) return;
    /* in edit mode even a clear pad shows where its controls are */
    alpha = (u32)(clampi(g_edit && g_vpad.opacity < 50 ? 50 : g_vpad.opacity, 0, 100) * 255 / 100);
    if (g_edit) edit_draw(l, safe);
    for (c = 0; c < SNX_VPAD_COUNT; ++c) {
        if (!g_vpad.at[c].shown) continue;
        box(c, safe, r);
        if (INFO[c].kind == KIND_DPAD)
            dpad_draw(l, r, alpha);
        else if (INFO[c].kind == KIND_STICK)
            stick_draw(l, c, r, safe, alpha);
        else
            ui_sprite(l, INFO[c].sprite + (int)(g_held >> c & 1), r[0], r[1], r[2], r[3], alpha, 0);
    }
}
