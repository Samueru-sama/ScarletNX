/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Included by cpu_helpers.c. Guest floating-point state is explicit and all
   operations consume raw IEEE bits. Never quiet an SNaN through a host cast. */
static u64 fp_input(CPU* c, u64 bits, int width) {
    u64 sign = 1ull << (width - 1);
    u64 mag = bits & (sign - 1);
    int frac = width == 64 ? 52 : 23;
    if (mag && !(mag >> frac) && (c->fpcr & (1u << 24))) {
        c->fpsr |= 1u << 7;
        return bits & sign;
    }
    return bits;
}
/* 0 = number, 1 = QNaN, 2 = SNaN. Never classify by a host FP comparison. */
static int fp_nanclass(u64 bits, int width) {
    int frac = width == 64 ? 52 : 23;
    u64 inf = width == 64 ? 0x7FF0000000000000ull : 0x7F800000ull;
    u64 mag = bits & ((1ull << (width - 1)) - 1);
    return mag <= inf ? 0 : bits & (1ull << (frac - 1)) ? 1 : 2;
}
/* Numeric ordering of IEEE encodings, with +0 == -0. */
static int fp_order(u64 a, u64 b, int width) {
    u64 sign = 1ull << (width - 1);
    if (a == b || !((a | b) & (sign - 1))) return 0;
    if ((a ^ b) & sign) return a & sign ? -1 : 1;
    return (a < b) ^ !!(a & sign) ? -1 : 1;
}
u32 snx_fp_compare(CPU* c, u64 a, u64 b, int width, int signal) {
    a = fp_input(c, a, width);
    b = fp_input(c, b, width);
    int na = fp_nanclass(a, width), nb = fp_nanclass(b, width);
    if (na || nb) {
        if (signal || na == 2 || nb == 2) c->fpsr |= 1u;
        return 3; /* unordered */
    }
    int order = fp_order(a, b, width);
    return order < 0 ? 8 : order == 0 ? 6 : 2;
}
u64 snx_fp_minmax(CPU* c, u64 a, u64 b, int width, int minimum, int numeric) {
    a = fp_input(c, a, width);
    b = fp_input(c, b, width);
    int na = fp_nanclass(a, width), nb = fp_nanclass(b, width);
    if (numeric) {
        if (na == 1 && !nb) return b;
        if (nb == 1 && !na) return a;
    }
    if (na || nb) {
        int frac = width == 64 ? 52 : 23;
        u64 quiet = 1ull << (frac - 1);
        if (na == 2 || nb == 2) c->fpsr |= 1u;
        if (c->fpcr & (1u << 25)) return width == 64 ? 0x7FF8000000000000ull : 0x7FC00000ull;
        /* FPProcessNaNs: signalling operands take priority over quiet ones. */
        return (na == 2 ? a : nb == 2 ? b : na ? a : b) | quiet;
    }
    if (!((a | b) & ((1ull << (width - 1)) - 1))) return minimum ? a | b : a & b;
    int order = fp_order(a, b, width);
    return minimum ? (order < 0 ? a : b) : (order > 0 ? a : b);
}

/* ARM FPRoundInt operates on the source format, before any host widening can
   quiet an SNaN. FRINTX alone reports inexact. A57 supports cumulative exception
   flags, not FP exception-enable traps. */
u64 snx_fp_round(CPU* c, u64 bits, int width, int rm, int exact) {
    int frac = width == 64 ? 52 : 23;
    int bias = width == 64 ? 1023 : 127;
    u64 sign = bits & (1ull << (width - 1));
    u64 mag = bits ^ sign;
    u64 inf = (u64)(2 * bias + 1) << frac;
    u64 quiet = 1ull << (frac - 1);
    if (mag > inf) {
        if (!(mag & quiet)) c->fpsr |= 1u;
        return c->fpcr & (1u << 25) ? inf | quiet : bits | quiet;
    }
    if (mag == inf || !mag) return bits;
    if (!(mag >> frac) && (c->fpcr & (1u << 24))) {
        c->fpsr |= 1u << 7;
        return sign;
    }
    int exponent = (int)(mag >> frac) - bias;
    if (exponent >= frac) return bits;
    u64 result;
    if (exponent < 0) {
        u64 half = (u64)(bias - 1) << frac;
        int up = rm == 0 ? mag > half : rm == 4 ? mag >= half : rm == 1 ? !sign : rm == 2 ? !!sign : 0;
        result = up ? (u64)bias << frac : 0;
    } else {
        int shift = frac - exponent;
        u64 unit = 1ull << shift, mask = unit - 1, rem = mag & mask;
        if (!rem) return bits;
        result = mag & ~mask;
        int up = rm == 0   ? rem > (unit >> 1) || (rem == (unit >> 1) && (result & unit))
                 : rm == 4 ? rem >= (unit >> 1)
                 : rm == 1 ? !sign
                 : rm == 2 ? !!sign
                           : 0;
        if (up) result += unit;
    }
    if (exact) c->fpsr |= 1u << 4;
    return sign | result;
}

#include "fp_arithmetic.c"
