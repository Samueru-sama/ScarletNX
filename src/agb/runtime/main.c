/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* scarletnx-agb: the AGB (Game Boy Advance) system type. It loads a GBA ROM onto the emulated board and reports
   what the board knows. The board is a stub: no CPU and no devices are emulated yet. */
#include "board/board.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    puts("Usage: scarletnx-agb [--] ROM.gba\n"
         "Runs a Game Boy Advance ROM on the AGB board.\n"
         "\n"
         "The AGB board is under construction: this build only recognizes the ROM and\n"
         "reports its header. Nothing is emulated yet.");
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char* path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) {
            ++i;
            if (i < argc) path = argv[i];
            break;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        }
        if (!path)
            path = argv[i];
        else {
            fprintf(stderr, "scarletnx-agb: unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!path) {
        usage();
        return 2;
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "scarletnx-agb: cannot open %s\n", path);
        return 1;
    }
    if (fseek(file, 0, SEEK_END)) {
        fprintf(stderr, "scarletnx-agb: cannot read %s\n", path);
        fclose(file);
        return 1;
    }
    long length = ftell(file);
    rewind(file);
    if (length < 0) length = 0;
    uint8_t* image = (uint8_t*)malloc((size_t)length);
    if (length && (!image || fread(image, 1, (size_t)length, file) != (size_t)length)) {
        fprintf(stderr, "scarletnx-agb: cannot read %s\n", path);
        free(image);
        fclose(file);
        return 1;
    }
    fclose(file);
    AgbBoard board;
    char error[256];
    agb_board_init(&board);
    if (agb_board_insert(&board, image, (size_t)length, error, sizeof error)) {
        fprintf(stderr, "scarletnx-agb: %s: %s\n", path, error);
        free(image);
        return 1;
    }
    free(image);
    const AgbCartridge* cartridge = &board.cartridge;
    printf("ScarletNX - %s (AGB)\n", cartridge->title);
    printf("cartridge: %s by %s, version %u, %zu bytes\n", cartridge->game_code, cartridge->maker_code,
           cartridge->version, cartridge->size);
    printf("entry: ARM branch to 0x%08X\n", (unsigned)(0x08000000u + cartridge->entry_to));
    puts("AGB board stub: no CPU and no devices are emulated yet.");
    agb_board_destroy(&board);
    return 0;
}
