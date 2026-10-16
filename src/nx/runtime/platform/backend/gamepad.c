/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#include "runtime/platform/gamepad.h"
#include "runtime/platform/log.h"
#include <SDL3/SDL.h>

static SDL_Gamepad* g_pads[SNX_GAMEPADS];
static struct {
    int background;
} g_cfg;

/* SDL drops a gamepad's presses while no window of the program has the focus, unless told otherwise; set before it
   starts its gamepads (the .ini is read first), and at once on a change */
static void background_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_cfg.background ? "1" : "0");
}
const IniOpt g_gamepad_ini[] = {{"gamepad_background", "0",
                                 "1 = the controllers still play the game while its window is not focused", INI_INT, 0,
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
            if (g_pads[i]) LOG("input: gamepad %s\n", SDL_GetGamepadName(g_pads[i]));
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

/* the dead zone as a circle, so a stick pushed along one axis keeps the small part of the other */
static void dz(i32* x, i32* y) {
    if ((i64)*x * *x + (i64)*y * *y < 7000ll * 7000) *x = *y = 0;
}
/* SDL's Y axes point down, the Switch's up */
static i32 up(i32 v) { return v == -32768 ? 32767 : -v; }

void gamepad_state(u64* buttons, i32 sticks[4]) {
    /* by position like a Switch pad: A right, B bottom, X top, Y left */
    static const struct {
        SDL_GamepadButton btn;
        int bit;
    } map[] = {{SDL_GAMEPAD_BUTTON_EAST, 0},          {SDL_GAMEPAD_BUTTON_SOUTH, 1},
               {SDL_GAMEPAD_BUTTON_NORTH, 2},         {SDL_GAMEPAD_BUTTON_WEST, 3},
               {SDL_GAMEPAD_BUTTON_LEFT_STICK, 4},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 5},
               {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 6}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7},
               {SDL_GAMEPAD_BUTTON_START, 10},        {SDL_GAMEPAD_BUTTON_BACK, 11},
               {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 12},    {SDL_GAMEPAD_BUTTON_DPAD_UP, 13},
               {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 14},   {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 15}};
    u64 b = 0;
    int i;
    size_t m;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0;
    for (i = 0; i < SNX_GAMEPADS; i++) {
        SDL_Gamepad* g = g_pads[i];
        if (!g) continue;
        for (m = 0; m < sizeof map / sizeof map[0]; m++)
            if (SDL_GetGamepadButton(g, map[m].btn)) b |= 1ull << map[m].bit;
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
