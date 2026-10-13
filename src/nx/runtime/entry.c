/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* executable entry point */
#include "runtime/main.h"
#include "driver.h"
#include <stdio.h>
int main(int argc, char** argv) {
    /* Keep progress visible when a GUI or another supervisor captures our pipes. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    int result = snx_driver(argc, argv);
    return result >= 0 ? result : snx_main(argc, argv);
}
