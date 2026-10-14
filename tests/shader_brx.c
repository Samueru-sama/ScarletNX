/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <string.h>

#define SCHED 0x001F8000FC0007E0ull
#define EXIT  0xE30000000007000Full

static int failures, reads, invalid_second;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static int table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    reads++;
    *value = offset == 0 || (offset == 4 && !invalid_second) ? 24 : 1024;
    return 1;
}

int main(void) {
    u64 code[64] = {0};
    code[10] = SCHED;
    code[11] = 0x3820000000170b0bull; /* IMNMX R11, R11, 1: two entries */
    code[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    code[13] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13] */
    code[14] = SCHED;
    code[15] = 0xe250000000070d0full; /* BRX R13 */
    code[16] = EXIT;
    code[18] = SCHED;
    code[19] = EXIT;
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = table;
    u32 len = 0;
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 20 * 8 && reads == 2, "clamped BRX reads only its two live table entries");

    reads = 0;
    invalid_second = 1;
    code[23] = EXIT;
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 24 * 8 && reads == 2, "invalid clamped target leaves BRX unresolved");
    invalid_second = 0;
    code[23] = 0;

    reads = 0;
    code[13] |= 1ull << 49; /* LDC.B64 is not the compiler's jump-table shape */
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 20 * 8 && reads == 3, "non-B32 loads do not borrow the table clamp");

    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = 0xe250000000070d0full; /* unresolved BRX */
    code[12] = EXIT;
    g_shader_cb_read = NULL;
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 13 * 8, "unresolved BRX stops after the final exit before zero padding");
    code[15] = EXIT;
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 16 * 8, "unresolved BRX includes live code beyond an earlier exit");
    g_shader_cb_read = old;
    return failures ? 1 : 0;
}
