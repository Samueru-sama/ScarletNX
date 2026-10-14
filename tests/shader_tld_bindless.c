/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The bindless TLD (TLD.B, 1101 1101): the handle in Rb, the rest as TLD. Ender Magnolia's words, offline (the
   translator and glslang, no ICD or GPU): before, the decoder didn't know the opcode and the shader was dropped */
#include "gpu/shader/shader.h"
#include "gpu/vk/spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED    0x001F8000FC0007E0ull
#define EXIT     0xE30000000007000Full
#define NOP      0x50B0000000070F00ull
/* Ender Magnolia's: TLD.B.LZ R12.Y (2D), [R12, R13], R15 (a fragment shader); TLD.B.LZ R4.X (1D), [R1], R4 (compute) */
#define TLD_B_2D 0xDD3A000120F70C0Cull
#define TLD_B_1D 0xDD3A000080470104ull

static int failures;
static void check(int ok, const char* what, const char* glsl) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n%s\n", what, glsl ? glsl : "(no GLSL)");
        failures++;
    }
}
/* MOV rd, c[bank][off]: where a bindless handle comes from */
static u64 mov_c(u32 rd, u32 bank, u32 off) {
    return 0x4C98078000070000ull | (u64)bank << 34 | (u64)(off / 4) << 20 | rd; /* predicate PT */
}

/* a shader of the stage with up to four instructions, then EXIT; its GLSL, compiled by glslang */
static char* translate(int stage, const u64* insn, int n, ShaderInfo* si, int* compiled) {
    static u64 code[64];
    memset(code, 0, sizeof code);
    int at = 10;
    for (int i = 0; i <= n; i++) {
        if ((at - 10) % 4 == 0) code[at++] = SCHED;
        code[at++] = i < n ? insn[i] : EXIT;
    }
    while ((at - 10) % 4) code[at++] = NOP;
    memset(si, 0, sizeof *si);
    char* glsl = shader_to_glsl((const u8*)code, sizeof code, stage, si);
    *compiled = 0;
    if (glsl && !si->unsupported) {
        size_t nw = 0;
        char err[2048] = "";
        u32* spv = vk_spirv_cached(glsl, stage, NULL, &nw, err, sizeof err);
        if (!spv) fprintf(stderr, "%s\n", err);
        *compiled = spv != NULL;
        free(spv);
    }
    return glsl;
}

int main(void) {
    ShaderInfo si;
    int ok;
    char* g = translate(ST_FS, (const u64[]){mov_c(15, 2, 0x40), TLD_B_2D}, 2, &si, &ok);
    check(!si.unsupported && ok && si.ntex == 1 && si.tex[0].bindless && si.tex[0].bl_cbuf == 2 &&
              si.tex[0].cbuf_off == 0x10 && si.tex[0].type == 1,
          "a 2D TLD.B: one bindless 2D texture, its handle c[2][0x40]", g);
    check(g && strstr(g, "texelFetch(tex0, ivec2(r12,r13), 0)"), "it fetches texel (R12, R13) at LOD 0", g);
    free(g);

    g = translate(ST_CS, (const u64[]){mov_c(4, 2, 0x48), TLD_B_1D}, 2, &si, &ok);
    check(!si.unsupported && ok && si.ntex == 1 && si.tex[0].bindless && si.tex[0].cbuf_off == 0x12 &&
              si.tex[0].type == 0,
          "a 1D TLD.B in a compute shader: one bindless 1D texture", g);
    free(g);

    /* a handle the translator can't follow stays untranslated, named */
    g = translate(ST_FS, (const u64[]){TLD_B_2D}, 1, &si, &ok);
    check(si.unsupported == 1 && !strncmp(si.first_unsupported, "TLD.B (untracked handle)", 24),
          "an untracked TLD.B handle is named", g);
    free(g);

    if (failures) return 1;
    puts("TLD.B: bindless texel fetches translate, from the handle in Rb, and compile");
    return 0;
}
