/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* executable entry point */
#include "nx/runtime/main.h"
#include "nx/runtime/driver/driver.h"
#ifdef SNX_WITH_GUI
#include "gui/gui.h"
#endif
#include <stdio.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IPHONE && defined(SNX_WITH_GUI)
/* iOS: SDL's main starts UIKit and calls this one as SDL_main; the menu's game runs here after it (src/gui/apple.m) */
#include "gui/apple.h"
#include <SDL3/SDL_main.h>
#define SNX_PLAY_AFTER_MENU 1
#endif
int main(int argc, char** argv) {
#ifdef SNX_WITH_GUI
    snx_menu = snx_gui_main;
#endif
    /* Keep progress visible when a GUI or another supervisor captures our pipes. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int result = snx_driver(argc, argv);
    result = result >= 0 ? result : snx_main(argc, argv);
#ifdef SNX_PLAY_AFTER_MENU
    {
        int played_argc;
        char** played = gui_apple_played(&played_argc);
        /* the menu only opened (main.c's open_menu): the runtime has not started, and starts once for this game */
        if (played) result = snx_main(played_argc, played);
    }
#endif
    return result;
}
