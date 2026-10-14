/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "board.h"
#include <string.h>

void agb_board_init(AgbBoard* board) { memset(board, 0, sizeof(*board)); }

int agb_board_insert(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size) {
    AgbCartridge cartridge;
    if (agb_cartridge_load(&cartridge, image, size, error, error_size)) return -1;
    agb_cartridge_unload(&board->cartridge);
    board->cartridge = cartridge;
    board->has_cartridge = 1;
    return 0;
}

void agb_board_destroy(AgbBoard* board) {
    agb_cartridge_unload(&board->cartridge);
    memset(board, 0, sizeof(*board));
}
