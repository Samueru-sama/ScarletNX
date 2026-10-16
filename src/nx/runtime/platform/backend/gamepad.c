/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#include "runtime/platform/gamepad.h"
#include "runtime/platform/log.h"
#include <SDL3/SDL.h>
#include <math.h>

static SDL_Gamepad* g_pads[SNX_GAMEPADS];
static struct {
    int labels, deadzone, background;
} g_cfg = {1, 20, 0};

/* SDL drops a gamepad's presses while no window of the program has the focus, unless told otherwise; set before it
   starts its gamepads (the .ini is read first), and at once on a change */
static void background_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_cfg.background ? "1" : "0");
}
const IniOpt g_gamepad_ini[] = {
    {"gamepad_buttons", "1",
     "A controller's A, B, X and Y: 1 = as their labels say, 0 = by position, as on the console's", INI_INT, 0,
     &g_cfg.labels, 0, 1, NULL},
    {"gamepad_deadzone", "20", "The sticks' dead zone, in percent of their travel", INI_INT, 0, &g_cfg.deadzone, 0, 90,
     NULL},
    {"gamepad_background", "0", "1 = the controllers still play the game while its window is not focused", INI_INT, 0,
     &g_cfg.background, 0, 1, background_set},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL}};
int gamepad_ini(const char* key, const char* value) {
    const IniOpt* o = ini_find(g_gamepad_ini, key);
    if (o) ini_read(o, value);
    return o != NULL;
}

static int is_open(SDL_JoystickID id) {
    int i;
    for (i = 0; i < SNX_GAMEPADS; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) return 1;
    return 0;
}
void gamepad_open(u32 id) {
    int i;
    if (is_open(id)) return;
    for (i = 0; i < SNX_GAMEPADS; i++)
        if (!g_pads[i]) {
            g_pads[i] = SDL_OpenGamepad(id);
            if (g_pads[i])
                LOG("input: gamepad %s (%s)\n", SDL_GetGamepadName(g_pads[i]),
                    SDL_GetGamepadStringForType(SDL_GetGamepadType(g_pads[i])));
            return;
        }
}
void gamepad_close(u32 id) {
    SDL_JoystickID* ids;
    int i, n = 0;
    for (i = 0; i < SNX_GAMEPADS; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
        }
    /* a fifth pad connected while four were open takes the free place */
    ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (i = 0; i < n; i++) gamepad_open(ids[i]);
    SDL_free(ids);
}

/* the dead zone as a circle, so a stick pushed along one axis keeps the small part of the other, and the travel past
   it spread over the whole range, so the stick starts from nothing and still reaches the edge (a square gate's corner
   is past it) */
static void dz(i32* x, i32* y) {
    double r = sqrt((double)*x * *x + (double)*y * *y), z = g_cfg.deadzone * 32767.0 / 100, k;
    if (r <= z) {
        *x = *y = 0;
        return;
    }
    k = ((r < 32767 ? r : 32767) - z) / (32767 - z) * 32767 / r;
    *x = (i32)(*x * k);
    *y = (i32)(*y * k);
}
/* SDL's Y axes point down, the Switch's up */
static i32 up(i32 v) { return v == -32768 ? 32767 : -v; }

/* the console's controller's buttons (HidNpadButton bits) where they sit: A right, B bottom, X top, Y left */
static const struct {
    SDL_GamepadButton btn;
    int bit;
} MAP[] = {
    {SDL_GAMEPAD_BUTTON_EAST, 0},          {SDL_GAMEPAD_BUTTON_SOUTH, 1},          {SDL_GAMEPAD_BUTTON_NORTH, 2},
    {SDL_GAMEPAD_BUTTON_WEST, 3},          {SDL_GAMEPAD_BUTTON_LEFT_STICK, 4},     {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 5},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 6}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7}, {SDL_GAMEPAD_BUTTON_START, 10},
    {SDL_GAMEPAD_BUTTON_BACK, 11},         {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 12},     {SDL_GAMEPAD_BUTTON_DPAD_UP, 13},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 14},   {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 15}};
/* a button's HidNpadButton bit, -1 for none. A face button as its label says (SDL knows each pad's), the symbols as
   the letters at their places on the most common layout: Cross is A, Circle B, Square X and Triangle Y */
static int bit_of(SDL_Gamepad* g, SDL_GamepadButton b) {
    size_t m;
    if (g_cfg.labels) {
        switch (SDL_GetGamepadButtonLabel(g, b)) {
            case SDL_GAMEPAD_BUTTON_LABEL_A:
            case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return 0;
            case SDL_GAMEPAD_BUTTON_LABEL_B:
            case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return 1;
            case SDL_GAMEPAD_BUTTON_LABEL_X:
            case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return 2;
            case SDL_GAMEPAD_BUTTON_LABEL_Y:
            case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return 3;
            default: break;
        }
    }
    for (m = 0; m < sizeof MAP / sizeof MAP[0]; m++)
        if (MAP[m].btn == b) return MAP[m].bit;
    return -1;
}
u64 gamepad_button(u32 id, int button) {
    SDL_Gamepad* g = SDL_GetGamepadFromID(id);
    int bit = g ? bit_of(g, (SDL_GamepadButton)button) : -1;
    return bit < 0 ? 0 : 1ull << bit;
}

void gamepad_state(u64* buttons, i32 sticks[4]) {
    u64 b = 0;
    int i;
    size_t m;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0;
    for (i = 0; i < SNX_GAMEPADS; i++) {
        SDL_Gamepad* g = g_pads[i];
        if (!g) continue;
        for (m = 0; m < sizeof MAP / sizeof MAP[0]; m++)
            if (SDL_GetGamepadButton(g, MAP[m].btn)) b |= 1ull << bit_of(g, MAP[m].btn);
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) b |= 1ull << 8;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) b |= 1ull << 9;
        if (!sticks[0] && !sticks[1]) {
            sticks[0] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
            sticks[1] = up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
            dz(&sticks[0], &sticks[1]);
        }
        if (!sticks[2] && !sticks[3]) {
            sticks[2] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
            sticks[3] = up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
            dz(&sticks[2], &sticks[3]);
        }
    }
    *buttons = b;
}
