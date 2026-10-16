/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The per-page write generations behind gpu_mem_dirty_since: exact per page, at any distance and any range size */
#include "nx/gpu/maxwell/write_pages.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    WritePages* t = calloc(1, sizeof(WritePages));
    const u64 tex = 0x500000000ull, tex_size = 16u << 20; /* a 16 MiB texture */
    u64 g;
    if (!t) return 1;
    check(!write_pages_since(t, 0, tex, tex_size), "an empty table has no writes");
    /* constant buffer updates 256 MiB and 1 GiB away shared the texture's slots in the hashed table */
    write_pages_stamp(t, tex + (256u << 20) + 0x40, 0x100, 1);
    write_pages_stamp(t, tex - (1u << 30), 0x10000, 2);
    for (g = 3; g < 5000; g++) write_pages_stamp(t, 0x100000000ull + (g & 63) * 0x1000, 4, g);
    check(!write_pages_since(t, 0, tex, tex_size), "writes elsewhere leave a large range clean");
    write_pages_stamp(t, tex + tex_size - 4, 4, 5000);
    check(write_pages_since(t, 4999, tex, tex_size), "a write in the last page of a large range is seen");
    check(!write_pages_since(t, 5000, tex, tex_size), "a write before since is not");
    check(!write_pages_since(t, 4999, tex, tex_size - 0x1000), "a range ending before the page is clean");
    write_pages_stamp(t, tex + tex_size - 0x800, 0x1000, 5001); /* across two leaves */
    check(write_pages_since(t, 5000, tex + tex_size, 0x1000), "a write is seen in every page it covers");
    check(write_pages_since(t, 5000, tex, tex_size), "and in the leaf it starts in");
    check(!write_pages_since(t, 5000, tex, 0x1000), "an unwritten page of a written leaf is clean");
    check(!write_pages_since(t, 5001, 0, ~0ull), "the whole address space after the latest write is clean");
    check(write_pages_since(t, 5000, 0, ~0ull), "the whole address space sees the latest write");
    write_pages_stamp(t, 1ull << 40, 0x1000, 5002);
    check(write_pages_since(t, 5001, tex, 1), "a write past 2^40 reads as covering every page");
    if (!failures) puts("Write generations are kept per page");
    return failures ? 1 : 0;
}
