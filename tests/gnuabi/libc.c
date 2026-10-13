/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* Optional static-libc integration fixture: compile separately with -static. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    unsigned char* buffer = malloc(16384);
    if (!buffer || argc < 1 || !argv[0]) return 1;
    memset(buffer, 0x5a, 16384);
    unsigned sum = 0;
    for (unsigned i = 0; i < 16384; ++i) sum += buffer[i];
    printf("static libc: %u\n", sum);
    free(buffer);
    return 0;
}
