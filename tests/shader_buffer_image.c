/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Surface operations on 1D buffer images in compute shaders, offline (the translator and glslang, no ICD or GPU):
   Ender Magnolia's SULD.D.B32, SUST.P and SUATOM.ADD, words as the game has them, become accesses to a storage
   buffer bound after the global ones (bimg<k>), and the GLSL compiles. Before, each one left its shader untranslated */
#include "gpu/shader/shader.h"
#include "gpu/vk/spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED      0x001F8000FC0007E0ull
#define EXIT       0xE30000000007000Full
#define NOP        0x50B0000000070F00ull
/* Ender Magnolia's: SULD.D.B32 R2, [R5], R6 (bindless); SUST.P.RGBA [R0], R4, R1; SUATOM.ADD.SD32 R2, [R5], R2, R7 */
#define SULD_B32   0xEB10030200480502ull
#define SUST_P     0xEB20008200F00004ull
#define SUATOM_ADD 0xEA70039200210502ull

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}
/* MOV rd, c[bank][off]: where a bindless handle comes from */
static u64 mov_c(u32 rd, u32 bank, u32 off) {
    return 0x4C98078000000000ull | (u64)bank << 34 | (u64)(off / 4) << 20 | rd;
}
/* w with the bits at pos, n wide, set to v */
static u64 with(u64 w, int pos, int n, u64 v) { return (w & ~(((1ull << n) - 1) << pos)) | v << pos; }

/* a compute shader of up to six instructions, then EXIT; its GLSL, which glslang must compile */
static char* translate(const u64* insn, int n, ShaderInfo* si, const char* what) {
    static u64 code[64];
    memset(code, 0, sizeof code);
    int at = 10;
    for (int i = 0; i <= n; i++) {
        if ((at - 10) % 4 == 0) code[at++] = SCHED;
        code[at++] = i < n ? insn[i] : EXIT;
    }
    while ((at - 10) % 4) code[at++] = NOP;
    memset(si, 0, sizeof *si);
    char* glsl = shader_to_glsl((const u8*)code, sizeof code, ST_CS, si);
    if (glsl && !si->unsupported) {
        size_t nw = 0;
        char err[2048] = "";
        u32* spv = vk_spirv_cached(glsl, ST_CS, NULL, &nw, err, sizeof err);
        if (!spv) fprintf(stderr, "%s: %s\n%s\n", what, err, glsl);
        check(spv != NULL, what);
        free(spv);
    }
    return glsl;
}
static int has(const char* glsl, const char* s) { return glsl && strstr(glsl, s); }
/* a check on a translation, which shows the GLSL when it fails */
static void expect(const char* glsl, int ok, const char* what) {
    check(ok, what);
    if (!ok && glsl) fprintf(stderr, "%s\n", glsl);
}

int main(void) {
    ShaderInfo si;
    char* g;

    g = translate((const u64[]){mov_c(6, 2, 0x40), SULD_B32}, 2, &si, "SULD.D.B32");
    check(!si.unsupported && si.nbimg == 1, "SULD.D.B32 on a buffer image translates");
    check(si.bimg[0].bindless && si.bimg[0].bl_cbuf == 2 && si.bimg[0].cbuf_off == 0x10 && !si.bimg[0].written,
          "its handle is c[2][0x40], read only");
    expect(g,
           has(g, "readonly buffer S5_B0") && has(g, "int o_ = (int(r5) * 4) >> 2;") && has(g, "r2 = bimg0.d[o_ + 0];"),
           "a 32-bit element: word r5 of bimg0");
    free(g);

    g = translate((const u64[]){mov_c(1, 2, 0x40), SUST_P}, 2, &si, "SUST.P");
    check(!si.unsupported && si.nbimg == 1 && si.bimg[0].written, "SUST.P on a buffer image translates, written");
    check(has(g, "snx_bb[0]") && has(g, "snx_v2r(vec4(uintBitsToFloat(r4), uintBitsToFloat(r5), uintBitsToFloat(r6), "
                                        "uintBitsToFloat(r7)), z_, snx_bc[0])"),
          "a texel of the header's format from R4-R7");
    free(g);

    g = translate((const u64[]){mov_c(7, 2, 0x48), SUATOM_ADD}, 2, &si, "SUATOM.ADD");
    check(!si.unsupported && si.nbimg == 1 && si.bimg[0].written && si.bimg[0].cbuf_off == 0x12,
          "SUATOM.ADD on a buffer image translates, written");
    check(has(g, "r2 = atomicAdd(bimg0.d[int(r5)], r2);"), "an atomic add on word r5, the old value to R2");
    free(g);

    /* the same handle is one buffer image, read and written */
    g = translate((const u64[]){mov_c(6, 2, 0x40), SULD_B32, mov_c(1, 2, 0x40), SUST_P}, 4, &si, "SULD and SUST");
    check(!si.unsupported && si.nbimg == 1 && si.bimg[0].written && !has(g, "readonly buffer S5_B0"),
          "SULD and SUST through one handle share a writable buffer image");
    free(g);

    /* two handles, two buffer images */
    g = translate((const u64[]){mov_c(6, 2, 0x40), SULD_B32, mov_c(7, 2, 0x48), SUATOM_ADD}, 4, &si, "two handles");
    check(!si.unsupported && si.nbimg == 2 && !si.bimg[0].written && si.bimg[1].written && has(g, "bimg1"),
          "a load and an atomic through two handles are two buffer images");
    free(g);

    /* elements below a word: S8 sign-extends the byte at r5 */
    g = translate((const u64[]){mov_c(6, 2, 0x40), with(SULD_B32, 20, 3, 1)}, 2, &si, "SULD.D.S8");
    check(!si.unsupported && has(g, "uint s_ = uint(o_ & 3) * 8u;") && has(g, "<< 24u) >> 24)"),
          "SULD.D.S8 sign-extends its byte");
    free(g);

    /* SUST.D.B64: two words */
    g = translate((const u64[]){mov_c(1, 2, 0x40), with(with(SUST_P, 52, 1, 1), 20, 4, 5)}, 2, &si, "SUST.D.B64");
    expect(g, !si.unsupported && has(g, "bimg0.d[o_ + 0] = r4;") && has(g, "bimg0.d[o_ + 1] = r5;"),
           "SUST.D.B64 stores two words");
    free(g);

    /* SUST.D.U16: a half word through atomics on its bits */
    g = translate((const u64[]){mov_c(1, 2, 0x40), with(with(SUST_P, 52, 1, 1), 20, 4, 2)}, 2, &si, "SUST.D.U16");
    check(!si.unsupported && has(g, "atomicAnd(bimg0.d[o_ >> 2], ~m_); atomicOr("), "SUST.D.U16 keeps its neighbours");
    free(g);

    /* signed MIN (S32) and INC have no single atomic: a compare-and-swap loop */
    g = translate((const u64[]){mov_c(7, 2, 0x48), with(with(SUATOM_ADD, 29, 4, 1), 51, 3, 1)}, 2, &si,
                  "SUATOM.MIN.S32");
    check(!si.unsupported && has(g, "atomicCompSwap(bimg0.d[int(r5)], o_, uint(min(int(o_), int(v_))))"),
          "SUATOM.MIN.S32 compares signed");
    free(g);
    g = translate((const u64[]){mov_c(7, 2, 0x48), with(SUATOM_ADD, 29, 4, 3)}, 2, &si, "SUATOM.INC");
    check(!si.unsupported && has(g, "o_ >= v_ ? 0u : o_ + 1u"), "SUATOM.INC wraps at the operand");
    free(g);
    g = translate((const u64[]){mov_c(7, 2, 0x48), with(SUATOM_ADD, 29, 4, 1)}, 2, &si, "SUATOM.MIN.U32");
    check(!si.unsupported && has(g, "r2 = atomicMin(bimg0.d[int(r5)], r2);"),
          "SUATOM.MIN.SD32 is unsigned, as in Eden");
    free(g);

    /* still left out: a 2D image's atomic, a 64-bit one, a clamp mode */
    g = translate((const u64[]){mov_c(7, 2, 0x48), with(SUATOM_ADD, 33, 3, 3)}, 2, &si, "SUATOM on a 2D image");
    check(si.unsupported == 1 && si.nbimg == 0, "SUATOM on a 2D image stays untranslated");
    free(g);
    g = translate((const u64[]){mov_c(7, 2, 0x48), with(SUATOM_ADD, 51, 3, 2)}, 2, &si, "SUATOM.U64");
    check(si.unsupported == 1, "a 64-bit SUATOM stays untranslated");
    free(g);
    g = translate((const u64[]){mov_c(7, 2, 0x48), with(SUATOM_ADD, 49, 2, 1)}, 2, &si, "SUATOM with a clamp");
    check(si.unsupported == 1, "a SUATOM with a clamp mode stays untranslated");
    free(g);

    if (failures) return 1;
    puts("buffer images: SULD, SUST and SUATOM translate to storage buffer accesses that glslang compiles");
    return 0;
}
