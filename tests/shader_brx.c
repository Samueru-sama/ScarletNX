/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/shader/shader.h"
#include <stdio.h>
#include <string.h>

#define SCHED 0x001F8000FC0007E0ull
#define EXIT  0xE30000000007000Full
#define NOP   0x50B0000000070F00ull

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
    u32 targets[64];
    reads = 0;
    check(shader_brx_targets(code, 15, 64, targets, 64) == 2 && reads == 2 && targets[0] == 19 && targets[1] == 19,
          "a clamped BRX table gives exactly its two targets");

    reads = 0;
    invalid_second = 1;
    code[23] = EXIT;
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 24 * 8 && reads == 2, "invalid clamped target leaves BRX unresolved");
    invalid_second = 0;
    code[23] = 0;

    reads = 0;
    code[13] |= 1ull << 49; /* LDC.B64: 8-byte entries whose low words the BRX would take, not a word table */
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 20 * 8 && reads == 0, "a non-B32 load leaves the BRX unresolved");

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

    u64 distant[64] = {0};
    for (u32 i = 10; i <= 49; i++) distant[i] = (i - 10) % 4 ? NOP : SCHED;
    distant[11] = 0x3820000000170b0bull; /* clamp R11 to two entries */
    distant[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    distant[13] = 0xef94000000070d0dull; /* LDC.B32 R13, c[0][R13] */
    distant[45] = 0xe250000000070d0full; /* BRX R13, 32 instructions after LDC */
    distant[49] = EXIT;
    g_shader_cb_read = table;
    reads = 0;
    shader_hash((const u8*)distant, sizeof distant, &len);
    check(len == 50 * 8 && reads == 2, "straight-line distant LDC resolves clamped BRX");
    distant[31] = 0x5c9807800ff7000dull; /* MOV R13, RZ invalidates the table value */
    reads = 0;
    shader_hash((const u8*)distant, sizeof distant, &len);
    check(reads == 0, "a later write to the BRX register rejects distant LDC");
    distant[31] = NOP;
    distant[13] |= 1ull << 49;
    reads = 0;
    shader_hash((const u8*)distant, sizeof distant, &len);
    check(reads == 0, "distant BRX needs a verified 32-bit table load");
    g_shader_cb_read = old;
    return failures ? 1 : 0;
}
