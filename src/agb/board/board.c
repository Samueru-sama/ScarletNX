/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "board.h"
#include <string.h>

#define AGB_IO_IF 0x202 /* the interrupt flags register */

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

uint16_t agb_io_read16(AgbBoard* board, uint32_t address) {
    uint32_t offset = (address >> 1) & (AGB_IO_COUNT - 1);
    if (offset * 2 <= 0x5F) return agb_ppu_read(board, offset * 2);
    return board->io[offset];
}

void agb_io_write16(AgbBoard* board, uint32_t address, uint16_t value) {
    uint32_t offset = (address >> 1) & (AGB_IO_COUNT - 1);
    if (offset * 2 <= 0x5F) {
        agb_ppu_write(board, offset * 2, value);
        return;
    }
    board->io[offset] = value;
}

void agb_irq_request(AgbBoard* board, uint32_t mask) { board->io[AGB_IO_IF / 2] |= (uint16_t)mask; }
