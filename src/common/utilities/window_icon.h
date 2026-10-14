/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_WINDOW_ICON_H
#define SNX_WINDOW_ICON_H
#include <SDL3/SDL.h>

/* the program's icon, decoded from the PNG built in (cmake/window_icon.cmake); NULL if it could not be */
SDL_Surface* snx_window_icon_surface(void);
/* Gives a window the program's icon where the window system takes it from the program. Windows takes the
   executable's own (runtime.rc), with every size, so nothing is set there */
void snx_window_icon(SDL_Window* window);
#endif
