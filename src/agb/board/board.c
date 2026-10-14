/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "board.h"
#include <string.h>

void agb_board_init(AgbBoard* board) {
    memset(board, 0, sizeof(*board));
    agb_memory_init(&board->memory);
}

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
    agb_memory_free(&board->memory);
    memset(board, 0, sizeof(*board));
}

/* ---- the I/O window ---- */

uint16_t agb_io_read16(AgbBoard* board, uint32_t address) {
    uint32_t offset = (address >> 1) & (AGB_IO_COUNT - 1), byte = offset * 2;
    if (byte <= 0x5F) return agb_ppu_read(board, byte);
    if (byte >= 0x60 && byte <= 0xA7) return agb_apu_read(board, byte);
    if (byte >= 0xB0 && byte <= 0xDF) return agb_dma_read(board, byte);
    if (byte >= 0x100 && byte <= 0x10F) return agb_timer_read(board, byte);
    if (byte == 0x130) return 0x3FF; /* KEYINPUT: every button released (HID lands later) */
    if (byte == 0x200) return board->irq_ie;
    if (byte == 0x202) return board->irq_if;
    if (byte == 0x208) return board->irq_ime;
    return board->io[offset];
}

void agb_io_write16(AgbBoard* board, uint32_t address, uint16_t value) {
    uint32_t offset = (address >> 1) & (AGB_IO_COUNT - 1), byte = offset * 2;
    if (byte <= 0x5F) {
        agb_ppu_write(board, byte, value);
        return;
    }
    if (byte >= 0x60 && byte <= 0xA7) {
        agb_apu_write(board, byte, value);
        return;
    }
    if (byte >= 0xB0 && byte <= 0xDF) {
        agb_dma_write(board, byte, value);
        return;
    }
    if (byte >= 0x100 && byte <= 0x10F) {
        agb_timer_write(board, byte, value);
        return;
    }
    if (byte == 0x200) {
        board->irq_ie = value;
        return;
    }
    if (byte == 0x202) {
        board->irq_if &= (uint16_t)~value; /* one bits acknowledge the interrupt flags */
        return;
    }
    if (byte == 0x208) {
        board->irq_ime = value & 1;
        return;
    }
    board->io[offset] = value;
}

/* Recompiled code writes the register window through the flat map. The sync applies those writes to the
   devices and refreshes the whole window with what the registers read back, so the CPU sees live values. */
void agb_io_sync(AgbBoard* board, uint8_t* io_window) {
    for (uint32_t i = 0; i < AGB_IO_COUNT; ++i) {
        uint16_t value = (uint16_t)(io_window[i * 2] | io_window[i * 2 + 1] << 8);
        uint16_t read;
        if (value != board->io_shadow[i]) agb_io_write16(board, 0x04000000 + i * 2, value);
        read = agb_io_read16(board, 0x04000000 + i * 2);
        board->io_shadow[i] = read;
        io_window[i * 2] = (uint8_t)read;
        io_window[i * 2 + 1] = (uint8_t)(read >> 8);
    }
}

void agb_irq_request(AgbBoard* board, uint32_t mask) {
    board->irq_if |= (uint16_t)mask;
    board->cpu_halted = 0; /* any raised flag wakes a halted CPU */
}
