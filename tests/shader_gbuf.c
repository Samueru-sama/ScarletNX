/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Global memory through storage buffers: which c0 words the translator takes as buffer addresses. A word becomes a
   buffer only when a global load or store goes through the register that holds it; deko3d's fragment buffers sit at
   c0[0x730] and its LDG takes the address with LDC.64. The words are the ones edenprobe's shaders compile to. */
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED 0x001F8000FC0007E0ull
#define EXIT  0xE30000000007000Full
#define NOP   0x50B0000000070F00ull

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* a fragment shader of up to five instructions, then EXIT */
static void translate(const u64* insn, int n, ShaderInfo* si) {
    static u64 code[64];
    memset(code, 0, sizeof code);
    int at = 10;
    for (int i = 0; i <= n; i++) {
        if ((at - 10) % 4 == 0) code[at++] = SCHED;
        code[at++] = i < n ? insn[i] : EXIT;
    }
    while ((at - 10) % 4) code[at++] = NOP;
    memset(si, 0, sizeof *si);
    free(shader_to_glsl((const u8*)code, sizeof code, ST_FS, si));
}

int main(void) {
    ShaderInfo si;
    {
        const u64 p[] = {0xEF9500007307FF00ull, 0xEED4200000070000ull}; /* LDC.64 R0, c[0x0][0x730]; LDG.E R0, [R0] */
        translate(p, 2, &si);
        check(!si.unsupported && si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x730, "deko3d's LDC.64 then LDG");
    }
    {
        /* IADD R0, R1, c[0x0][0x730]; IADD.X R1, RZ, c[0x0][0x734]; STG.E [R0], R2 */
        const u64 p[] = {0x4C1080001CC70100ull, 0x4C1008001CD7FF01ull, 0xEEDC200000070002ull};
        translate(p, 3, &si);
        check(!si.unsupported && si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x730 && (si.gbuf_wmask & 1),
              "deko3d's IADD pair then STG, written");
    }
    {
        const u64 p[] = {0x4C98078018870002ull}; /* MOV R2, c[0x0][0x620], no global access */
        translate(p, 1, &si);
        check(!si.unsupported && si.ngbuf == 0, "a c0 word no load or store uses takes no buffer");
    }
    {
        const u64 p[] = {0x4C98078014470002ull, 0xEED4200000070200ull}; /* MOV R2, c[0x0][0x510]; LDG.E R0, [R2] */
        translate(p, 2, &si);
        check(!si.unsupported && si.ngbuf == 1 && si.gbuf_cbuf_off[0] == 0x510 && !(si.gbuf_wmask & 1),
              "NVN's fragment buffer, read only");
    }
    if (failures) return 1;
    puts("global memory: buffers taken on use, from MOV, IADD and LDC.64 of c0, NVN's and deko3d's places");
    return 0;
}
