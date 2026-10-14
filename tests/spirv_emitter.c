/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "spirv_emitter_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    size_t words = 0;
    uint32_t* code = NULL;
    if (!strcmp(argv[1], "compute"))
        code = snx_test_spirv_compute(&words);
    else if (!strcmp(argv[1], "vertex"))
        code = snx_test_spirv_graphics(0, &words);
    else if (!strcmp(argv[1], "fragment"))
        code = snx_test_spirv_graphics(1, &words);
    else
        return 2;
    FILE* file = fopen(argv[2], "wb");
    if (!file) {
        free(code);
        return 1;
    }
    int ok = fwrite(code, sizeof(uint32_t), words, file) == words;
    if (fclose(file)) ok = 0;
    free(code);
    return ok ? 0 : 1;
}
