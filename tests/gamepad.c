/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The gamepads (runtime/platform/backend/gamepad.c) on SDL's virtual ones, under its dummy video driver: what a press,
   a trigger or a stick gives the game and the game's menu, on the common layouts, with the window's focus or without,
   and a fifth pad taking the place of one that goes */
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
enum { A = 1, B = 2, X = 4, Y = 8, ZL = 1 << 8, ZR = 1 << 9, PLUS = 1 << 10, LEFT = 1 << 12, UP = 1 << 13 };
/* the virtual pads, by the vendor and product SDL tells their layout from: only they are read, a real pad plugged in
   stays out */
#define XBOX     0x045e, 0x02ea
#define SWITCH   0x057e, 0x2009
#define SONY     0x054c, 0x09cc
#define GAMECUBE 0x057e, 0x0337
#define PADS_IDS "0x045e/0x02ea,0x057e/0x2009,0x054c/0x09cc,0x057e/0x0337"

/* the backend's event loop (sdl3.c's video_poll), for the gamepads */
static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_GAMEPAD_ADDED)
            gamepad_open(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
            gamepad_close(e.gdevice.which);
}
/* what each pad's motors were last told: low (the heavy one) | high << 16 */
static Uint32 motors[8];
static int pads;
static bool SDLCALL rumble(void* pad, Uint16 low, Uint16 high) {
    *(Uint32*)pad = low | (Uint32)high << 16;
    return true;
}
/* a pad with every gamepad button and axis (button i is SDL_GamepadButton i) and two motors, at rest */
static SDL_Joystick* attach(Uint16 vendor, Uint16 product) {
    SDL_VirtualJoystickDesc d;
    SDL_Joystick* j;
    int a;
    SDL_INIT_INTERFACE(&d);
    d.Rumble = rumble;
    d.userdata = &motors[pads++];
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
    SDL_Joystick *xbox, *pro, *sony, *cube, *fifth;
    SDL_Window* w;
    i32 s[4];
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, PADS_IDS);
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    w = SDL_CreateWindow("gamepad", 64, 64, 0);
    CHECK(w != NULL && SDL_GetKeyboardFocus() == w);

    /* the face buttons as their labels say: A at the bottom on the most common layout */
    xbox = attach(XBOX);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == A);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == B);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_WEST) == X);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_NORTH) == Y);
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
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == 0);
    CHECK(gamepad_ini("gamepad_background", "1"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == A);
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, 0, s);
    CHECK(s[0] == 32767);
    sticks(xbox, 0, 0, s);
    CHECK(gamepad_ini("gamepad_background", "0") && !gamepad_ini("gamepad", "0"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == 0);
    SDL_ShowWindow(w);

    /* the sticks: up is up, a stick inside the dead zone (20%) is still, and the travel past it spans the range */
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, 0, s);
    CHECK(s[0] == 32767 && s[1] == 0 && s[2] == 0 && s[3] == 0);
    sticks(xbox, 0, SDL_JOYSTICK_AXIS_MIN, s);
    CHECK(s[0] == 0 && s[1] == 32767);
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, SDL_JOYSTICK_AXIS_MIN, s); /* a square gate's corner, on the edge */
    CHECK(s[0] == s[1] && s[0] > 23160 && s[0] < 23180);
    sticks(xbox, 3000, -3000, s);
    CHECK(s[0] == 0 && s[1] == 0);
    sticks(xbox, 16384, 0, s);
    CHECK(s[0] > 12280 && s[0] < 12295 && s[1] == 0); /* (50% - 20%) / 80% */
    CHECK(gamepad_ini("gamepad_deadzone", "0"));
    sticks(xbox, 3000, -3000, s);
    CHECK(s[0] == 3000 && s[1] == 3000);
    CHECK(gamepad_ini("gamepad_deadzone", "50"));
    sticks(xbox, 16000, 0, s);
    CHECK(s[0] == 0);
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, 0, s);
    CHECK(s[0] == 32767);
    CHECK(gamepad_ini("gamepad_deadzone", "20"));
    sticks(xbox, 0, 0, s);

    /* the console's own layout, A on the right; the symbols as the letters where they sit on the common one, and the
       older console's big A in the middle and B on the left */
    pro = attach(SWITCH);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_NORTH) == X);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_WEST) == Y);
    sony = attach(SONY);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_SOUTH) == A);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_EAST) == B);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_WEST) == X);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_NORTH) == Y);
    cube = attach(GAMECUBE);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_SOUTH) == A);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_WEST) == B);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_EAST) == X);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_NORTH) == Y);
    /* the game's menu reads them the same: A selects, B goes back */
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_SOUTH) == A);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_EAST) == B);
    CHECK(gamepad_button(SDL_GetJoystickID(pro), SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_DPAD_UP) == UP);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_GUIDE) == 0);

    /* gamepad_buttons=0: by position on every pad, as on the console's controller: A right, B bottom, X top, Y left */
    CHECK(gamepad_ini("gamepad_buttons", "0"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_NORTH) == X);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_WEST) == Y);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(gamepad_ini("gamepad_buttons", "1") && !gamepad_ini("gamepad", "1"));

    /* the game's vibration on every pad: each motor the stronger of the left and right devices' bands, as strong as
       gamepad_rumble says */
    gamepad_rumble(0x4000u | 0x1000u << 16, 0x2000u | 0x8000u << 16);
    CHECK(motors[0] == (0x4000u | 0x8000u << 16) && motors[1] == motors[0] && motors[3] == motors[0]);
    CHECK(gamepad_ini("gamepad_rumble", "50"));
    gamepad_rumble(0x4000u | 0x1000u << 16, 0x2000u | 0x8000u << 16);
    CHECK(motors[0] == (0x2000u | 0x4000u << 16));
    CHECK(gamepad_ini("gamepad_rumble", "0")); /* never: a pad that shakes stops */
    gamepad_rumble(0xFFFFu | 0xFFFFu << 16, 0);
    CHECK(motors[0] == 0 && motors[2] == 0);
    CHECK(gamepad_ini("gamepad_rumble", "100"));
    gamepad_rumble(0xFFFFu, 0);
    CHECK(motors[0] == 0xFFFFu);
    gamepad_rumble(0, 0); /* the game stops it */
    CHECK(motors[0] == 0);

    /* every pad drives the one controller, and the first stick that is pushed wins */
    SDL_SetJoystickVirtualButton(xbox, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(pro, SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(buttons() == (A | B));
    SDL_SetJoystickVirtualButton(xbox, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(pro, SDL_GAMEPAD_BUTTON_SOUTH, false);
    sticks(pro, SDL_JOYSTICK_AXIS_MIN, 0, s);
    CHECK(s[0] <= -32767 && s[1] == 0);
    sticks(pro, 0, 0, s);

    /* four are read; a fifth takes the place of one that goes */
    fifth = attach(SWITCH);
    CHECK(with(fifth, SDL_GAMEPAD_BUTTON_EAST) == 0);
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(sony));
    pump();
    CHECK(with(fifth, SDL_GAMEPAD_BUTTON_EAST) == A);

    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("gamepad: %d checks\n", checks);
    return 0;
}
