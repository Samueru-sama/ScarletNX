/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ARMv8-A pseudocode witnesses, independent of the differential emulator.
   FPRoundInt, FPProcessNaNs, FPMax/Min[Num], FPCompare and FPUnpack.
   All operands/results are IEEE encodings, never host NaN literals. */
#include "cpu.h"
#include <fenv.h>
#include <stdio.h>

static int failures;
static void check(u64 actual, u64 expected, const char* label) {
    if (actual != expected) {
        fprintf(stderr, "%s: got %016llx expected %016llx\n", label, (unsigned long long)actual,
                (unsigned long long)expected);
        ++failures;
    }
}

static void rounding(void) {
    static const struct {
        int width, rm, exact;
        u32 fpcr;
        u64 input, output;
        u32 flags;
    } cases[] = {
        {32, 0, 0, 0, 0x3F000000, 0, 0},          /* +0.5 -> +0, ties even */
        {32, 0, 0, 0, 0xBF000000, 0x80000000, 0}, /* -0.5 -> -0 */
        {32, 0, 0, 0, 0x3FC00000, 0x40000000, 0}, /* 1.5 -> 2 */
        {32, 0, 0, 0, 0x40200000, 0x40000000, 0}, /* 2.5 -> 2 */
        {32, 4, 0, 0, 0x40200000, 0x40400000, 0}, /* ties away -> 3 */
        {32, 4, 0, 0, 0xBF000000, 0xBF800000, 0},
        {32, 1, 0, 0, 1, 0x3F800000, 0}, /* smallest subnormal */
        {32, 2, 0, 0, 0x80000001, 0xBF800000, 0},
        {32, 3, 1, 0, 0x80000001, 0x80000000, 0x10},
        {32, 0, 1, 0, 0x3FC00000, 0x40000000, 0x10}, /* FRINTX inexact */
        {32, 0, 1, 0, 0x40000000, 0x40000000, 0},
        {32, 1, 1, 1u << 24, 1, 0, 0x80}, /* FZ: IDC, not IXC */
        {32, 2, 1, 1u << 24, 0x80000001, 0x80000000, 0x80},
        {32, 0, 1, 0, 0x7F800123, 0x7FC00123, 1}, /* SNaN -> quiet + IOC */
        {32, 0, 1, 0, 0xFFC00123, 0xFFC00123, 0}, /* QNaN sign/payload */
        {32, 0, 1, 1u << 25, 0xFF800123, 0x7FC00000, 1},
        {32, 0, 1, 1u << 25, 0xFFC00123, 0x7FC00000, 0},
        {32, 1, 1, 0, 0x7F7FFFFF, 0x7F7FFFFF, 0}, /* largest finite */
        {32, 4, 1, 0, 0xFF800000, 0xFF800000, 0}, /* -infinity */
        {64, 0, 0, 0, 0xBFE0000000000000ull, 0x8000000000000000ull, 0},
        {64, 0, 0, 0, 0xBFE0000000000001ull, 0xBFF0000000000000ull, 0},
        {64, 4, 0, 0, 0xBFDFFFFFFFFFFFFFull, 0x8000000000000000ull, 0},
        {64, 1, 1, 0, 1, 0x3FF0000000000000ull, 0x10},
        {64, 2, 1, 1u << 24, 0x8000000000000001ull, 0x8000000000000000ull, 0x80},
        {64, 3, 1, 0, 0x7FEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0},
        {64, 0, 1, 0, 0xFFF0000000000123ull, 0xFFF8000000000123ull, 1},
        {64, 0, 1, 1u << 25, 0xFFF8000000000123ull, 0x7FF8000000000000ull, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        CPU c = {0};
        c.fpcr = cases[i].fpcr;
        c.fpsr = 0x08000002; /* unrelated cumulative flags must survive */
        check(snx_fp_round(&c, cases[i].input, cases[i].width, cases[i].rm, cases[i].exact), cases[i].output, "round");
        check(c.fpsr, 0x08000002 | cases[i].flags, "round FPSR");
    }
}

static void selection_and_comparison(void) {
    for (int width = 32; width <= 64; width += 32) {
        u64 sign = 1ull << (width - 1);
        u64 inf = width == 32 ? 0x7F800000ull : 0x7FF0000000000000ull;
        u64 quiet = width == 32 ? 0x400000ull : 0x8000000000000ull;
        u64 qnan = sign | inf | quiet | 123, snan = inf | 456;
        CPU c = {0};
        check(snx_fp_minmax(&c, 0, sign, width, 0, 0), 0, "max signed zero");
        check(snx_fp_minmax(&c, 0, sign, width, 1, 0), sign, "min signed zero");
        check(snx_fp_minmax(&c, qnan, inf, width, 0, 1), inf, "maxnum QNaN");
        check(snx_fp_minmax(&c, inf, qnan, width, 1, 1), inf, "minnum QNaN");
        check(c.fpsr, 0, "quiet NaN FPSR");
        check(snx_fp_minmax(&c, qnan, snan, width, 0, 1), snan | quiet, "SNaN priority");
        check(c.fpsr, 1, "SNaN IOC");
        c.fpcr = 1u << 25;
        c.fpsr = 0;
        check(snx_fp_minmax(&c, qnan, inf, width, 0, 0), inf | quiet, "default NaN");
        check(snx_fp_minmax(&c, qnan, inf, width, 0, 1), inf, "numeric before DN");
        check(snx_fp_compare(&c, qnan, 0, width, 0), 3, "FCMP unordered");
        check(c.fpsr, 0, "FCMP quiet");
        check(snx_fp_compare(&c, qnan, 0, width, 1), 3, "FCMPE unordered");
        check(c.fpsr, 1, "FCMPE IOC");
        c.fpsr = 0;
        check(snx_fp_compare(&c, snan, 0, width, 0), 3, "FCMP signalling");
        check(c.fpsr, 1, "FCMP SNaN IOC");
        c.fpcr = 1u << 24;
        c.fpsr = 0;
        check(snx_fp_minmax(&c, sign | 1, 1, width, 1, 0), sign, "min FZ zeros");
        check(c.fpsr, 0x80, "min IDC");
        check(snx_fp_compare(&c, 1, sign | 1, width, 0), 6, "compare FZ zeros");
        c.fpcr = 0;
        check(snx_fp_compare(&c, sign | 1, 1, width, 0), 8, "compare tiny numbers");
    }
}

static void vector_rounding(void) {
    CPU c = {0};
    c.v[0].s[0] = 0x7F800123;
    c.v[0].s[1] = 0xBF000000;
    c.v[0].s[2] = 0x3FC00000;
    c.v[0].s[3] = 0xFFC00456;
    check(snx_simd(&c, NULL, 0x4E218800), 1, "FRINTN V0.4S,V0.4S");
    check(c.v[0].s[0], 0x7FC00123, "vector SNaN");
    check(c.v[0].s[1], 0x80000000, "vector signed zero");
    check(c.v[0].s[2], 0x40000000, "vector tie");
    check(c.v[0].s[3], 0xFFC00456, "vector QNaN");
    check(c.fpsr, 1, "vector cumulative IOC");
    check(snx_simd(&c, NULL, 0x5E218820), 0, "reserved scalar SIMD FRINT");
    check(snx_simd(&c, NULL, 0x0E618820), 0, "reserved FRINT .1D");
}

int main(void) {
    static const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    int original = fegetround();
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        if (fesetround(modes[i])) return 2;
        rounding();
        selection_and_comparison();
        vector_rounding();
    }
    fesetround(original);
    if (!failures) puts("ARMv8 FP witnesses passed under all four host rounding modes");
    return failures != 0;
}
