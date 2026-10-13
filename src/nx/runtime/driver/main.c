/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "driver.h"
#include <stdio.h>
/* Standalone build for the host tools. The full runtime uses the same driver. */
int main(int argc, char** argv) {
    int result = snx_driver(argc, argv);
    if (result >= 0) return result;
    fputs("Pass a dump file, -x, -r or -h. This build contains host tools only.\n", stderr);
    return 2;
}
