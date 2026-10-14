/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the AGB (Game Boy Advance) board. A stub so far: it holds the cartridge and nothing else. */
#ifndef SNX_AGB_BOARD_H
#define SNX_AGB_BOARD_H
#include "../cartridge/cartridge.h"

typedef struct {
    AgbCartridge cartridge;
    int has_cartridge;
} AgbBoard;

void agb_board_init(AgbBoard* board);
/* Takes a copy of the image: returns 0, or -1 with a message when the image is not a GBA ROM. */
int agb_board_insert(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size);
void agb_board_destroy(AgbBoard* board);
#endif
