/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Included by fp.c. Integer implementations of DDI0487A.k's FPUnpack,
   FPRound, arithmetic and conversions. No host FP operations, environment,
   process-global rounding state or endian-dependent representation is used.
   Unsigned 128-bit arithmetic is also required by the CPU's integer helpers. */
typedef unsigned __int128 FpWide;
typedef struct FpValue {
    u64 bits, sig;
    int exp, sign, kind; /* value = (-1)^sign * sig * 2^exp */
} FpValue;
enum { FPV_ZERO, FPV_FINITE, FPV_INF, FPV_QNAN, FPV_SNAN };
static int fp_frac(int w) { return w == 64 ? 52 : w == 32 ? 23 : 10; }
static int fp_bias(int w) { return w == 64 ? 1023 : w == 32 ? 127 : 15; }
static u64 fp_sign(int w) { return 1ull << (w - 1); }
static u64 fp_inf(int w) { return (u64)(2 * fp_bias(w) + 1) << fp_frac(w); }
static u64 fp_default(int w) { return fp_inf(w) | (1ull << (fp_frac(w) - 1)); }
static int fp_top(FpWide v) {
    u64 hi = (u64)(v >> 64);
    return hi ? 127 - (int)clz64(hi) : 63 - (int)clz64((u64)v);
}
static FpWide fp_jam(FpWide v, int n) {
    if (!n) return v;
    if (n >= 128) return v != 0;
    return (v >> n) | ((v << (128 - n)) != 0);
}
static FpValue fp_unpack(CPU* c, u64 bits, int w) {
    if (w != 16) bits = fp_input(c, bits, w);
    int f = fp_frac(w), bias = fp_bias(w);
    u64 frac = bits & ((1ull << f) - 1);
    int e = (int)((bits & (fp_sign(w) - 1)) >> f);
    FpValue v = {bits, frac, (e ? e : 1) - bias - f, !!(bits & fp_sign(w)), FPV_FINITE};
    if (e == 2 * bias + 1 && !(w == 16 && (c->fpcr & (1u << 26))))
        v.kind = !frac ? FPV_INF : frac & (1ull << (f - 1)) ? FPV_QNAN : FPV_SNAN;
    else if (e)
        v.sig |= 1ull << f;
    else if (!frac)
        v.kind = FPV_ZERO;
    return v;
}
/* All operands must be unpacked before selection, including input-denormal flags. */
static u64 fp_nan(CPU* c, FpValue a, int w) {
    if (a.kind == FPV_SNAN) c->fpsr |= 1u;
    return c->fpcr & (1u << 25) ? fp_default(w) : a.bits | (1ull << (fp_frac(w) - 1));
}
static FpValue fp_nan2(FpValue a, FpValue b) {
    return a.kind == FPV_SNAN ? a : b.kind == FPV_SNAN ? b : a.kind == FPV_QNAN ? a : b;
}
static u64 fp_invalid(CPU* c, int w) {
    c->fpsr |= 1u;
    return fp_default(w);
}
/* rm = RNE,+inf,-inf,zero,ties-away,odd. Return a rounded magnitude. */
static FpWide fp_quantize(FpWide v, int shift, int sign, int rm, int* inexact) {
    if (shift <= 0) {
        *inexact = 0;
        return v << -shift;
    }
    FpWide q, rem, half;
    int cmp;
    if (shift >= 128) {
        q = 0;
        rem = v;
        half = (FpWide)1 << 127;
        cmp = shift > 128 ? -1 : v < half ? -1 : v == half ? 0 : 1;
    } else {
        q = v >> shift;
        rem = v & (((FpWide)1 << shift) - 1);
        half = (FpWide)1 << (shift - 1);
        cmp = rem < half ? -1 : rem == half ? 0 : 1;
    }
    *inexact = rem != 0;
    if (rm == 5) return q | (rem != 0);
    int up = rm == 0   ? cmp > 0 || (cmp == 0 && (q & 1))
             : rm == 4 ? cmp >= 0
             : rm == 1 ? !sign && rem
             : rm == 2 ? sign && rem
                       : 0;
    return q + up;
}
static u64 fp_overflow(CPU* c, int w, int sign, int rm) {
    c->fpsr |= (1u << 2) | (1u << 4);
    int inf = rm == 0 || rm == 4 || (rm == 1 && !sign) || (rm == 2 && sign);
    return (sign ? fp_sign(w) : 0) | (fp_inf(w) - !inf);
}
static u64 fp_pack(CPU* c, FpWide sig, int exp, int sign, int w, int rm) {
    u64 s = sign ? fp_sign(w) : 0;
    if (!sig) return s;
    int f = fp_frac(w), bias = fp_bias(w), top = fp_top(sig);
    int e = exp + top, tiny = e < 1 - bias;
    if (tiny && w != 16 && (c->fpcr & (1u << 24))) {
        c->fpsr |= 1u << 3; /* FZ: UFC even for an exact tiny result, no IXC. */
        return s;
    }
    int be = tiny ? 0 : e + bias, inexact;
    FpWide m = fp_quantize(sig, top - f + (tiny ? 1 - bias - e : 0), sign, rm, &inexact);
    if (tiny && inexact) c->fpsr |= 1u << 3; /* tininess before rounding */
    if (!be && m >= ((FpWide)1 << f)) be = 1;
    if (m >= ((FpWide)1 << (f + 1))) {
        m >>= 1;
        be++;
    }
    if (w == 16 && (c->fpcr & (1u << 26))) {
        if (be >= 32) {
            c->fpsr |= 1u; /* Alternative half overflow is invalid, not inexact. */
            return s | 0x7FFF;
        }
    } else if (be >= 2 * bias + 1)
        return fp_overflow(c, w, sign, rm);
    if (inexact) c->fpsr |= 1u << 4;
    return s | ((u64)be << f) | ((u64)m & ((1ull << f) - 1));
}
/* Keep the leading bit at 120. Exact products occupy at most 106 bits.
   Alignment can discard bits only when exponents are far enough apart that
   cancellation cannot expose them at the destination's (at most 53) bits.
   Right-jamming retains their effect on ties and directed rounding. */
static u64 fp_sum(CPU* c, FpWide a, int ea, int sa, FpWide b, int eb, int sb, int w, int scale) {
    int rm = (c->fpcr >> 22) & 3;
    if (!a && !b) return (sa == sb ? sa : rm == 2) ? fp_sign(w) : 0;
    if (!a) return fp_pack(c, b, eb + scale, sb, w, rm);
    if (!b) return fp_pack(c, a, ea + scale, sa, w, rm);
    int na = 120 - fp_top(a), nb = 120 - fp_top(b);
    a <<= na;
    ea -= na;
    b <<= nb;
    eb -= nb;
    if (ea < eb) {
        FpWide t = a;
        a = b;
        b = t;
        int t2 = ea;
        ea = eb;
        eb = t2;
        t2 = sa;
        sa = sb;
        sb = t2;
    }
    b = fp_jam(b, ea - eb);
    if (sa == sb)
        a += b;
    else if (a >= b)
        a -= b;
    else {
        a = b - a;
        sa = sb;
    }
    if (!a) sa = rm == 2;
    return fp_pack(c, a, ea + scale, sa, w, rm);
}
static u64 fp_fused(CPU* c, u64 add, u64 x, u64 y, int w, int scale) {
    FpValue d = fp_unpack(c, add, w), a = fp_unpack(c, x, w), b = fp_unpack(c, y, w);
    int bad_product = (a.kind == FPV_INF && b.kind == FPV_ZERO) || (b.kind == FPV_INF && a.kind == FPV_ZERO);
    if (d.kind == FPV_QNAN && bad_product) return fp_invalid(c, w);
    if (d.kind >= FPV_QNAN || a.kind >= FPV_QNAN || b.kind >= FPV_QNAN) return fp_nan(c, fp_nan2(d, fp_nan2(a, b)), w);
    int sp = a.sign ^ b.sign, inf = a.kind == FPV_INF || b.kind == FPV_INF;
    if (bad_product || (inf && d.kind == FPV_INF && d.sign != sp)) return fp_invalid(c, w);
    if (inf || d.kind == FPV_INF) return ((inf ? sp : d.sign) ? fp_sign(w) : 0) | fp_inf(w);
    return fp_sum(c, (FpWide)a.sig * b.sig, a.exp + b.exp, sp, d.sig, d.exp, d.sign, w, scale);
}
u64 snx_fp_fma(CPU* c, u64 add, u64 a, u64 b, int w) { return fp_fused(c, add, a, b, w, 0); }
u64 snx_fp_binary(CPU* c, u64 x, u64 y, int w, int op) {
    u64 signbit = fp_sign(w);
    if (op == SNX_FP_ABD) return snx_fp_binary(c, x, y, w, SNX_FP_SUB) & (signbit - 1);
    if (op == SNX_FP_RECPS || op == SNX_FP_RSQRTS) x ^= signbit;
    FpValue a = fp_unpack(c, x, w), b = fp_unpack(c, y, w);
    if (a.kind >= FPV_QNAN || b.kind >= FPV_QNAN) return fp_nan(c, fp_nan2(a, b), w);
    int sa = a.sign, sb = b.sign, sp = sa ^ sb, f = fp_frac(w), rm = (c->fpcr >> 22) & 3;
    u64 psign = sp ? signbit : 0;
    if (op == SNX_FP_ADD || op == SNX_FP_SUB) {
        sb ^= op == SNX_FP_SUB;
        if (a.kind == FPV_INF && b.kind == FPV_INF && sa != sb) return fp_invalid(c, w);
        if (a.kind == FPV_INF || b.kind == FPV_INF) return ((a.kind == FPV_INF ? sa : sb) ? signbit : 0) | fp_inf(w);
        return fp_sum(c, a.sig, a.exp, sa, b.sig, b.exp, sb, w, 0);
    }
    int bad_product = (a.kind == FPV_INF && b.kind == FPV_ZERO) || (b.kind == FPV_INF && a.kind == FPV_ZERO);
    if (op == SNX_FP_RECPS || op == SNX_FP_RSQRTS) {
        u64 two = (u64)(fp_bias(w) + 1) << f;
        int half = op == SNX_FP_RSQRTS;
        if (bad_product) return half ? two - (1ull << (f - 1)) : two;
        return fp_fused(c, two | (half ? 1ull << (f - 1) : 0), x, y, w, -half);
    }
    if (op == SNX_FP_MUL || op == SNX_FP_MULX) {
        if (bad_product) return op == SNX_FP_MULX ? psign | ((u64)(fp_bias(w) + 1) << f) : fp_invalid(c, w);
        if (a.kind == FPV_INF || b.kind == FPV_INF) return psign | fp_inf(w);
        return fp_pack(c, (FpWide)a.sig * b.sig, a.exp + b.exp, sp, w, rm);
    }
    /* Division keeps at least eleven guard bits beyond double precision. */
    if ((a.kind == FPV_ZERO && b.kind == FPV_ZERO) || (a.kind == FPV_INF && b.kind == FPV_INF)) return fp_invalid(c, w);
    if (a.kind == FPV_INF) return psign | fp_inf(w);
    if (b.kind == FPV_INF) return psign;
    if (b.kind == FPV_ZERO) {
        c->fpsr |= 1u << 1;
        return psign | fp_inf(w);
    }
    if (!a.sig) return psign;
    int na = f - fp_top(a.sig), nb = f - fp_top(b.sig);
    FpWide num = (FpWide)(a.sig << na) << 64;
    u64 den = b.sig << nb;
    FpWide q = num / den;
    q |= num % den != 0;
    return fp_pack(c, q, a.exp - na - b.exp + nb - 64, sp, w, rm);
}
u64 snx_fp_convert(CPU* c, u64 bits, int src, int dst, int rm) {
    FpValue a = fp_unpack(c, bits, src);
    u64 sign = a.sign ? fp_sign(dst) : 0;
    int ahp = dst == 16 && (c->fpcr & (1u << 26));
    if (a.kind >= FPV_QNAN) {
        if (a.kind == FPV_SNAN || ahp) c->fpsr |= 1u;
        if (ahp) return sign;
        if (c->fpcr & (1u << 25)) return fp_default(dst);
        u64 payload = bits & ((1ull << fp_frac(src)) - 1);
        int shift = fp_frac(dst) - fp_frac(src);
        payload = shift >= 0 ? payload << shift : payload >> -shift;
        return sign | fp_default(dst) | payload;
    }
    if (a.kind == FPV_INF) {
        if (ahp) {
            c->fpsr |= 1u;
            return sign | 0x7FFF;
        }
        return sign | fp_inf(dst);
    }
    return fp_pack(c, a.sig, a.exp, a.sign, dst, rm);
}
u64 snx_fp_from_int(CPU* c, u64 bits, int iw, int fw, int uns, int fbits) {
    bits &= bmask(iw);
    int sign = !uns && (bits & fp_sign(iw));
    u64 mag = sign ? (0 - bits) & bmask(iw) : bits;
    return fp_pack(c, mag, -fbits, sign, fw, (c->fpcr >> 22) & 3);
}
u64 snx_fp_to_int(CPU* c, u64 bits, int fw, int iw, int uns, int fbits, int rm) {
    FpValue a = fp_unpack(c, bits, fw);
    if (a.kind >= FPV_QNAN) {
        c->fpsr |= 1u;
        return 0;
    }
    int exp = a.exp + fbits, inexact = 0;
    FpWide mag;
    if (a.kind == FPV_INF || (a.sig && fp_top(a.sig) + exp >= 64))
        mag = (FpWide)1 << 64;
    else
        mag = fp_quantize(a.sig, -exp, a.sign, rm, &inexact);
    FpWide limit = uns ? (FpWide)bmask(iw) : ((FpWide)1 << (iw - 1)) - !a.sign;
    if ((uns && a.sign && mag) || mag > limit) {
        c->fpsr |= 1u;
        return a.sign ? (uns ? 0 : fp_sign(iw)) : (u64)limit;
    }
    if (inexact) c->fpsr |= 1u << 4;
    return (a.sign ? 0 - (u64)mag : (u64)mag) & bmask(iw);
}
/* ARM's 8-bit estimates, not a host reciprocal approximation. */
static u32 recip_est(u32 a) { return (((1u << 19) / (2 * a + 1)) + 1) >> 1; }
static u32 rsqrt_est(u32 a) {
    a = a < 256 ? 2 * a + 1 : 2 * ((a & ~1u) + 1);
    /* Same strict boundary as ARM's linear search, in at most nine steps.
       The scaled input is 257..1024, so all these products fit in u32. */
    u32 lo = 512, hi = 1024;
    while (hi - lo > 1) {
        u32 mid = (lo + hi) >> 1;
        if (a * mid * mid < (1u << 28))
            lo = mid;
        else
            hi = mid;
    }
    return (lo + 1) >> 1;
}
u32 urecpe32(u32 x) { return x & 0x80000000u ? (recip_est(x >> 23) | 0x100) << 23 : 0xFFFFFFFFu; }
u32 ursqrte32(u32 x) { return x & 0xC0000000u ? (rsqrt_est(x >> 23) | 0x100) << 23 : 0xFFFFFFFFu; }
u64 snx_fp_unary(CPU* c, u64 bits, int w, int op) {
    FpValue a = fp_unpack(c, bits, w);
    int f = fp_frac(w), bias = fp_bias(w), emax = 2 * bias + 1;
    u64 sign = a.sign ? fp_sign(w) : 0;
    if (a.kind >= FPV_QNAN) return fp_nan(c, a, w);
    int e = (int)((bits & (fp_sign(w) - 1)) >> f);
    if (op == SNX_FP_RECPX) return sign | ((u64)(e ? emax ^ e : emax - 1) << f);
    if (op == SNX_FP_RECPE) {
        if (a.kind == FPV_INF) return sign;
        if (a.kind == FPV_ZERO) {
            c->fpsr |= 1u << 1;
            return sign | fp_inf(w);
        }
        if (!e && a.sig < (1ull << (f - 2))) return fp_overflow(c, w, a.sign, (c->fpcr >> 22) & 3);
        if (e >= emax - 2 && (c->fpcr & (1u << 24))) {
            c->fpsr |= 1u << 3;
            return sign;
        }
        u64 frac = (bits & ((1ull << f) - 1)) << (52 - f);
        if (!e) {
            if (!(frac & (1ull << 51))) {
                e = -1;
                frac <<= 2;
            } else
                frac <<= 1;
            frac &= (1ull << 52) - 1;
        }
        int re = emax - 2 - e;
        u64 rf = (u64)(recip_est(256 + (u32)(frac >> 44)) & 255) << 44;
        if (re == 0)
            rf = (1ull << 51) | (rf >> 1);
        else if (re == -1) {
            rf = (1ull << 50) | (rf >> 2);
            re = 0;
        }
        return sign | ((u64)re << f) | (rf >> (52 - f));
    }
    if (a.kind == FPV_ZERO) {
        if (op == SNX_FP_SQRT) return sign;
        c->fpsr |= 1u << 1;
        return sign | fp_inf(w);
    }
    if (a.sign) return fp_invalid(c, w);
    if (a.kind == FPV_INF) return op == SNX_FP_SQRT ? fp_inf(w) : 0;
    if (op == SNX_FP_RSQRTE) {
        u64 frac = (bits & ((1ull << f) - 1)) << (52 - f);
        if (!e) {
            while (!(frac & (1ull << 51))) {
                frac <<= 1;
                e--;
            }
            frac = (frac << 1) & ((1ull << 52) - 1);
        }
        u32 sc = e & 1 ? 128 + (u32)(frac >> 45) : 256 + (u32)(frac >> 44);
        return ((u64)((3 * bias - 1 - e) / 2) << f) | ((u64)(rsqrt_est(sc) & 255) << (f - 8));
    }
    /* Restoring integer square root. An even scaling exponent preserves the
       exact radicand; the remainder supplies the sticky bit below 63 root bits. */
    int shift = 124 - fp_top(a.sig);
    if ((a.exp - shift) % 2) shift--;
    FpWide rem = (FpWide)a.sig << shift, root = 0, bit = (FpWide)1 << 124;
    while (bit) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else
            root >>= 1;
        bit >>= 2;
    }
    root |= rem != 0;
    return fp_pack(c, root, (a.exp - shift) / 2, 0, w, (c->fpcr >> 22) & 3);
}
