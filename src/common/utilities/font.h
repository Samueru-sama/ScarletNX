/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_FONT_H
#define SNX_GUI_FONT_H
#include <stdint.h>
extern const uint8_t gui_vga_font[256 * 16];
extern const uint16_t gui_codepage[256];
/* the glyph of a code point: the font is CP437, and what it lacks becomes '?' (the menu's and the game's own panel) */
unsigned char gui_glyph(uint32_t codepoint);
#endif
