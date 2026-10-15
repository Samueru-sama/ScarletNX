/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a fatal error's box says (runtime/platform/fatal_screen.c): the hint for each known error, the text cut into
   the box's rows at spaces, a long word cut, UTF-8 counted as characters, and those in the font's bytes */
#include "runtime/platform/fatal_screen.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char rows[8][FATAL_ROW];

int main(void) {
    int n;
    assert(strstr(fatal_hint("vulkan: the GPU device was lost (driver reset or crash)"), "driver reset it"));
    assert(strstr(fatal_hint("Unsupported instruction 0x1ee0f800 at 0x80021c4"), "does not translate"));
    assert(strstr(fatal_hint("Vulkan could not start (see the log). ScarletNX needs a GPU and driver with Vulkan 1.3."),
                  "Vulkan 1.3"));
    assert(strstr(fatal_hint("kernel: out of memory mapping 0x10000000 bytes"), "ran out of memory"));
    assert(!fatal_hint("svc 0x7f is not implemented"));

    /* at spaces, never past the width */
    n = fatal_wrap("vulkan: the GPU device was lost (driver reset or crash)", 20, rows, 8);
    assert(n == 4);
    assert(!strcmp(rows[0], "vulkan: the GPU"));
    assert(!strcmp(rows[1], "device was lost"));
    assert(!strcmp(rows[2], "(driver reset or"));
    assert(!strcmp(rows[3], "crash)"));
    n = fatal_wrap("a b", 20, rows, 8);
    assert(n == 1 && !strcmp(rows[0], "a b"));
    /* a word as wide as the row fills it, a longer one is cut */
    n = fatal_wrap("abcdefghij klm", 10, rows, 8);
    assert(n == 2 && !strcmp(rows[0], "abcdefghij") && !strcmp(rows[1], "klm"));
    n = fatal_wrap("abcdefghijklmno", 10, rows, 8);
    assert(n == 2 && !strcmp(rows[0], "abcdefghij") && !strcmp(rows[1], "klmno"));
    /* UTF-8: characters, not bytes, fill a row; one is never split */
    n = fatal_wrap("\xE2\x96\xA0\xE2\x96\xA0\xE2\x96\xA0 ab", 4, rows, 8);
    assert(n == 2 && !strcmp(rows[0], "\xE2\x96\xA0\xE2\x96\xA0\xE2\x96\xA0") && !strcmp(rows[1], "ab"));
    /* spaces at the edges and runs of them make no empty rows */
    n = fatal_wrap("   one     two   ", 3, rows, 8);
    assert(n == 2 && !strcmp(rows[0], "one") && !strcmp(rows[1], "two"));
    assert(fatal_wrap("", 10, rows, 8) == 0 && fatal_wrap("     ", 10, rows, 8) == 0);
    /* the count goes past max, the rows past it are not written */
    strcpy(rows[1], "kept");
    n = fatal_wrap("aa bb cc", 2, rows, 1);
    assert(n == 3 && !strcmp(rows[0], "aa") && !strcmp(rows[1], "kept"));
    /* the font's bytes: CP437 for what it has, '?' for the rest and for bytes that are no UTF-8 */
    {
        char out[32];
        fatal_cp437("caf\xC3\xA9 \xE2\x96\xA0 \xE6\x97\xA5", out, sizeof out);
        assert(!strcmp(out, "caf\x82 \xFE ?"));
        fatal_cp437("a\x80"
                    "b\xC3",
                    out, sizeof out);
        assert(!strcmp(out, "a?b?"));
        fatal_cp437("abcdef", out, 4);
        assert(!strcmp(out, "abc"));
    }
    puts("fatal screen: hints, rows and the font's bytes");
    return 0;
}
