/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the screen of a fatal error says (backend/sdl3.c draws it in the game's window): what the error means, for the
   errors a player can act on or report, and the text cut into the screen's rows */
#ifndef SNX_FATAL_SCREEN_H
#define SNX_FATAL_SCREEN_H
#include <stddef.h>

#define FATAL_ROW 1024 /* bytes of one row: cols code points of up to 4 bytes */

/* what msg (the text fatal() logged) means, in a sentence; NULL for an error with no known cause */
const char* fatal_hint(const char* msg);
/* text in rows of cols code points at most, broken at a space when one is in the row, else cut: up to max rows into
   rows. Returns the rows the text takes, which may be more than max (those past it are not written) */
int fatal_wrap(const char* text, int cols, char (*rows)[FATAL_ROW], int max);
/* text in the font's bytes (CP437, gui_glyph), for ui_text: what the font lacks becomes '?' */
void fatal_cp437(const char* text, char* out, size_t size);
#endif
