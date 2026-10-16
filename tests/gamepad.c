/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The gamepads (runtime/platform/backend/gamepad.c) on SDL's virtual ones, under its dummy video driver: what a press,
   a trigger or a stick gives the game, with the window's focus or without, and a fifth pad taking the place of one
   that goes */
#include "runtime/platform/gamepad.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

int g_verbose;
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}

static int checks;
#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "gamepad.c:%d: check failed: %s\n", __LINE__, #e);                                         \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* HidNpadButton bits */
enum { A = 1, B = 2, X = 4, Y = 8, ZL = 1 << 8, ZR = 1 << 9, PLUS = 1 << 10, LEFT = 1 << 12 };
/* the virtual pads, by the vendor and product SDL tells their layout from: only they are read, a real pad plugged in
   stays out */
#define XBOX     0x045e, 0x02ea
#define SWITCH   0x057e, 0x2009
#define PADS_IDS "0x045e/0x02ea,0x057e/0x2009"

/* the backend's event loop (sdl3.c's video_poll), for the gamepads */
static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_GAMEPAD_ADDED)
            gamepad_open(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
            gamepad_close(e.gdevice.which);
}
/* a pad with every gamepad button and axis (button i is SDL_GamepadButton i), at rest */
static SDL_Joystick* attach(Uint16 vendor, Uint16 product) {
    SDL_VirtualJoystickDesc d;
    SDL_Joystick* j;
    int a;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = vendor;
    d.product_id = product;
    j = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&d));
    CHECK(j != NULL);
    /* a real pad reports its axes at rest as it opens, a virtual one when they are first set; a trigger rests at the
       bottom of the range */
    for (a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
        SDL_SetJoystickVirtualAxis(j, a, a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? SDL_JOYSTICK_AXIS_MIN : 0);
    pump();
    return j;
}
static u64 buttons(void) {
    u64 b;
    i32 s[4];
    pump();
    gamepad_state(&b, s);
    return b;
}
/* what the game gets with one button held */
static u64 with(SDL_Joystick* j, SDL_GamepadButton button) {
    u64 b;
    SDL_SetJoystickVirtualButton(j, button, true);
    b = buttons();
    SDL_SetJoystickVirtualButton(j, button, false);
    CHECK(buttons() == 0);
    return b;
}
static void sticks(SDL_Joystick* j, Sint16 lx, Sint16 ly, i32 s[4]) {
    u64 b;
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, lx);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, ly);
    pump();
    gamepad_state(&b, s);
}

int main(void) {
    SDL_Joystick *xbox, *pro, *more[2], *fifth;
    SDL_Window* w;
    i32 s[4];
    int i;
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, PADS_IDS);
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    w = SDL_CreateWindow("gamepad", 64, 64, 0);
    CHECK(w != NULL && SDL_GetKeyboardFocus() == w);

    /* the face buttons by position, as on the console's controller: A right, B bottom, X top, Y left */
    xbox = attach(XBOX);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_NORTH) == X);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_WEST) == Y);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_START) == PLUS);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_DPAD_LEFT) == LEFT);
    /* a trigger is ZL or ZR past a quarter of its travel */
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    CHECK(buttons() == (ZL | ZR));
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    CHECK(buttons() == 0);
    /* the window without the focus (the dummy driver's hidden one has none): SDL drops the pad's presses, unless
       gamepad_background=1 */
    SDL_HideWindow(w);
    CHECK(SDL_GetKeyboardFocus() == NULL);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == 0);
    CHECK(gamepad_ini("gamepad_background", "1"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == A);
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, 0, s);
    CHECK(s[0] == 32767);
    sticks(xbox, 0, 0, s);
    CHECK(gamepad_ini("gamepad_background", "0") && !gamepad_ini("gamepad", "0"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == 0);
    SDL_ShowWindow(w);

    /* the sticks: up is up, and a stick inside the dead zone is still */
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, SDL_JOYSTICK_AXIS_MIN, s);
    CHECK(s[0] == 32767 && s[1] == 32767 && s[2] == 0 && s[3] == 0);
    sticks(xbox, 3000, -3000, s);
    CHECK(s[0] == 0 && s[1] == 0);
    sticks(xbox, 0, 0, s);

    /* every pad drives the one controller, and the first stick that is pushed wins */
    pro = attach(SWITCH);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_EAST) == A);
    SDL_SetJoystickVirtualButton(xbox, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualButton(pro, SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(buttons() == (A | B));
    SDL_SetJoystickVirtualButton(xbox, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(pro, SDL_GAMEPAD_BUTTON_SOUTH, false);
    sticks(pro, SDL_JOYSTICK_AXIS_MIN, 0, s);
    CHECK(s[0] == -32768 && s[1] == 0);
    sticks(pro, 0, 0, s);

    /* four are read; a fifth takes the place of one that goes */
    for (i = 0; i < 2; i++) more[i] = attach(XBOX);
    fifth = attach(SWITCH);
    CHECK(with(fifth, SDL_GAMEPAD_BUTTON_EAST) == 0);
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(more[0]));
    pump();
    CHECK(with(fifth, SDL_GAMEPAD_BUTTON_EAST) == A);

    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("gamepad: %d checks\n", checks);
    return 0;
}
