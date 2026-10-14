/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the AGB board: the console the recompiled ROM runs on. It holds the cartridge, the memory map and the
   devices behind the I/O registers. */
#ifndef SNX_AGB_BOARD_H
#define SNX_AGB_BOARD_H
#include "../cartridge/cartridge.h"
#include "memory.h"

typedef struct AgbBoard {
    AgbCartridge cartridge;
    AgbMemory memory;
    uint16_t io[AGB_IO_COUNT]; /* the I/O register file; devices claim registers here */
    int has_cartridge;
} AgbBoard;

void agb_board_init(AgbBoard* board);
/* Takes a copy of the image: returns 0, or -1 with a message when the image is not a GBA ROM. */
int agb_board_insert(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size);
void agb_board_destroy(AgbBoard* board);

/* The I/O window at 0x04000000. For now every register is plain storage; the devices replace their
   registers with real behaviour as they land. */
uint16_t agb_io_read16(AgbBoard* board, uint32_t address);
void agb_io_write16(AgbBoard* board, uint32_t address, uint16_t value);
#endif
