/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* F2I's modifiers: bit 45 negates its operand and bit 49 takes its absolute value. The console's compiler builds
   signed % as F2I.TRUNC with bit 45 set, then IMAD q * b + a, so a swap turns 4 % 3 into 7. */
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED 0x001F8000FC0007E0ull
#define EXIT  0xE30000000007000Full
#define NOP   0x50B0000000070F00ull
#define F2I   0x5CB0118000070A00ull /* F2I.U32.TRUNC R0, R0 */

static int failures;
static void expect(u64 word, const char* has, const char* lacks, const char* what) {
    static u64 code[64];
    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = word;
    code[12] = EXIT;
    code[13] = NOP;
    ShaderInfo si;
    memset(&si, 0, sizeof si);
    char* glsl = shader_to_glsl((const u8*)code, sizeof code, ST_FS, &si);
    if (!glsl || si.unsupported || !strstr(glsl, has) || (lacks && strstr(glsl, lacks))) {
        fprintf(stderr, "FAIL: %s\n%s\n", what, glsl ? glsl : "(no GLSL)");
        failures++;
    }
    free(glsl);
}

int main(void) {
    expect(F2I | 1ull << 45, "trunc((-", "abs(", "bit 45 negates");
    expect(F2I | 1ull << 49, "trunc(abs(", "(-", "bit 49 takes the absolute value");
    expect(F2I | 1ull << 45 | 1ull << 49, "trunc((-abs(", NULL, "both: the negated absolute value");
    if (failures) return 1;
    puts("F2I: NEG at bit 45, ABS at bit 49");
    return 0;
}
