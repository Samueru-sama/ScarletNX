/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Scalar floating point: the FP patterns of a64_templates.inc, as ir_fp.c builds them for LLVM. Their C calls the
   integer-only FP of cpu_helpers.c (snx_fp_*), in the runtime library; the fast paths use the host's IEEE arithmetic
   where it gives the helpers' exact result and flags (ir.h explains when). */
#include "jit.h"
#include "cpu.h"
#include <string.h>

typedef gcc_jit_rvalue* V;
typedef gcc_jit_type* T;
#define F(lo, n) ((w >> (lo)) & ((1UL << (n)) - 1))

#define ADD(a, b) jit_op(j, GCC_JIT_BINARY_OP_PLUS, a, b)
#define SUB(a, b) jit_op(j, GCC_JIT_BINARY_OP_MINUS, a, b)
#define MUL(a, b) jit_op(j, GCC_JIT_BINARY_OP_MULT, a, b)
#define DIV(a, b) jit_op(j, GCC_JIT_BINARY_OP_DIVIDE, a, b)
#define AND(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_AND, a, b)
#define OR(a, b)  jit_op(j, GCC_JIT_BINARY_OP_BITWISE_OR, a, b)
#define XOR(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_XOR, a, b)
#define SHL(a, b) jit_op(j, GCC_JIT_BINARY_OP_LSHIFT, a, b)
#define SHR(a, b) jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, a, b)
#define NEG(a)    jit_unary(j, GCC_JIT_UNARY_OP_MINUS, a)
#define EQ(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_EQ, a, b)
#define NE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_NE, a, b)
#define LT(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_LT, a, b)
#define LE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_LE, a, b)
#define GE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_GE, a, b)
#define U32(v)    jit_cast(j, v, j->t_u32)
#define U64(v)    jit_cast(j, v, j->t_u64)
#define K32(v)    jit_u32(j, v)
#define K64(v)    jit_u64(j, v)

/* ---- fast paths ---- */

typedef struct {
    T it, ft; /* the lane's uN and its float */
    unsigned width, mantissa, emax, precision;
} Shape;

static void shape(SnxJit* j, unsigned width, Shape* s) {
    s->width = width;
    s->it = width == 64 ? j->t_u64 : j->t_u32;
    s->ft = width == 64 ? j->t_f64 : j->t_f32;
    s->mantissa = width == 64 ? 52 : 23;
    s->emax = width == 64 ? 0x7FF : 0xFF;
    s->precision = s->mantissa + 1;
}
static V kint(SnxJit* j, const Shape* s, SnxU64 v) { return jit_k(j, s->it, v); }
static V kfloat(SnxJit* j, const Shape* s, double v) {
    return gcc_jit_context_new_rvalue_from_double(j->ctx, s->ft, v);
}
static V to_float(SnxJit* j, const Shape* s, V bits) { return jit_bitcast(j, jit_cast(j, bits, s->it), s->ft); }
static V to_bits(SnxJit* j, const Shape* s, V f) { return jit_bitcast(j, f, s->it); }
static V exponent(SnxJit* j, const Shape* s, V bits) {
    return AND(SHR(jit_cast(j, bits, s->it), kint(j, s, s->mantissa)), kint(j, s, s->emax));
}
/* lo <= exponent <= hi */
static V exponent_in(SnxJit* j, const Shape* s, V bits, unsigned lo, unsigned hi) {
    return LE(SUB(exponent(j, s, bits), kint(j, s, lo)), kint(j, s, hi - lo));
}
static V rounds_to_nearest(SnxJit* j) {
    return EQ(AND(jit_ld(j, offsetof(CPU, fpcr), j->t_u32), K32(3u << 22)), K32(0));
}
static V ixc_set(SnxJit* j) { return NE(AND(jit_ld(j, offsetof(CPU, fpsr), j->t_u32), K32(1u << 4)), K32(0)); }
static V flush_to_zero(SnxJit* j) { return NE(AND(jit_ld(j, offsetof(CPU, fpcr), j->t_u32), K32(1u << 24)), K32(0)); }
/* a math builtin of the lane's float type: name for double, namef for float */
static V math(SnxJit* j, const Shape* s, const char* name, int count, V* args) {
    char full[40];
    sprintf(full, "%s%s", name, s->width == 64 ? "" : "f");
    return jit_call(j, jit_builtin(j, full), count, args);
}
static V fma3f(SnxJit* j, const Shape* s, V a, V b, V c) {
    V args[3];
    args[0] = a, args[1] = b, args[2] = c;
    return math(j, s, "__builtin_fma", 3, args);
}
/* f != 0, true for NaN as LLVM's une */
static V nonzero(SnxJit* j, const Shape* s, V f) { return NE(f, kfloat(j, s, 0.0)); }

void jit_fp_fast(SnxJit* j, unsigned op, unsigned width, unsigned lanes, V const* a, V const* b, V const* add,
                 SnxJitFpFast* out) {
    Shape s;
    V ok, exact_ok, ixc, inexact = NULL;
    unsigned p, top, i;
    shape(j, width, &s);
    p = s.precision;
    top = s.emax - 1;
    ixc = jit_val(j, ixc_set(j));
    ok = rounds_to_nearest(j);
    exact_ok = op == SNX_JIT_FP_ADD || op == SNX_JIT_FP_SUB ? jit_true(j) : ixc;
    for (i = 0; i < lanes; ++i) {
        V fa = to_float(j, &s, a[i]), fb = b ? to_float(j, &s, b[i]) : NULL, r = NULL, lane_inexact = NULL, lane_ok;
        ok = jit_land(j, ok, exponent_in(j, &s, a[i], 1, top));
        if (b) ok = jit_land(j, ok, exponent_in(j, &s, b[i], 1, top));
        switch (op) {
            case SNX_JIT_FP_ADD:
            case SNX_JIT_FP_SUB: { /* TwoSum: the rounding error of a sum is a float, and exactly da+db */
                V y = jit_val(j, op == SNX_JIT_FP_SUB ? NEG(fb) : fb), bp, ap;
                fa = jit_val(j, fa);
                r = jit_val(j, ADD(fa, y));
                bp = jit_val(j, SUB(r, fa));
                ap = jit_val(j, SUB(r, bp));
                lane_inexact = nonzero(j, &s, ADD(SUB(fa, ap), SUB(y, bp)));
                break;
            }
            case SNX_JIT_FP_MUL:
                r = jit_val(j, MUL(fa, fb));
                if (j->fma) { /* a*b - r, exact while the result is at least 2^(emin+p) */
                    lane_inexact = nonzero(j, &s, fma3f(j, &s, fa, fb, NEG(r)));
                    exact_ok = jit_lor(j, exact_ok, exponent_in(j, &s, to_bits(j, &s, r), p + 2, top - 1));
                }
                break;
            case SNX_JIT_FP_DIV:
                r = jit_val(j, DIV(fa, fb));
                if (j->fma) { /* a - r*b, exact under the same bounds on a and r */
                    lane_inexact = nonzero(j, &s, fma3f(j, &s, NEG(r), fb, fa));
                    exact_ok = jit_lor(j, exact_ok,
                                       jit_land(j, exponent_in(j, &s, to_bits(j, &s, r), p + 2, top - 1),
                                                exponent_in(j, &s, a[i], p + 2, top - 1)));
                }
                break;
            case SNX_JIT_FP_SQRT:
                ok = jit_land(j, ok, GE(jit_cast(j, a[i], jit_sint(j, width)), jit_k(j, jit_sint(j, width), 0)));
                r = math(j, &s, "__builtin_sqrt", 1, &fa);
                if (j->fma) { /* a - r*r */
                    lane_inexact = nonzero(j, &s, fma3f(j, &s, NEG(r), r, fa));
                    exact_ok = jit_lor(j, exact_ok, exponent_in(j, &s, a[i], p + 2, top - 1));
                }
                break;
            default: { /* FMA: add + a*b, fused; whether it is exact isn't cheap to know, so only once IXC is set */
                V fadd = to_float(j, &s, add[i]);
                ok = jit_land(j, ok, exponent_in(j, &s, add[i], 1, top));
                if (j->fma)
                    r = fma3f(j, &s, fa, fb, fadd);
                else { /* no hardware FMA: never fast */
                    r = jit_val(j, ADD(fadd, MUL(fa, fb)));
                    exact_ok = jit_false(j);
                }
                break;
            }
        }
        out->value[i] = jit_val(j, to_bits(j, &s, r));
        /* no zero, denormal or smallest-binade result (tininess is detected before rounding), no overflow */
        lane_ok = exponent_in(j, &s, out->value[i], 2, top);
        ok = jit_land(j, ok, lane_ok);
        if (lane_inexact) inexact = inexact ? jit_lor(j, inexact, lane_inexact) : lane_inexact;
    }
    out->ok = jit_val(j, jit_land(j, ok, exact_ok));
    out->inexact = inexact ? jit_val(j, inexact) : NULL;
}

gcc_jit_block* jit_fp_branch(SnxJit* j, V ok, gcc_jit_block** slow) {
    gcc_jit_block *fast = jit_block(j), *join = jit_block(j);
    *slow = jit_block(j);
    jit_branch(j, jit_expect(j, ok, 1), fast, *slow);
    jit_enter(j, fast);
    return join;
}
void jit_fp_else(SnxJit* j, gcc_jit_block* join, gcc_jit_block* slow, V inexact) {
    if (inexact) { /* fpsr |= IXC */
        V fpsr = jit_ld(j, offsetof(CPU, fpsr), j->t_u32);
        jit_st(j, offsetof(CPU, fpsr), j->t_u32, OR(fpsr, SHL(U32(inexact), K32(4))));
    }
    jit_goto(j, join);
    jit_enter(j, slow);
}
void jit_fp_join(SnxJit* j, gcc_jit_block* join) {
    jit_goto(j, join);
    jit_enter(j, join);
}

/* the helpers' prototypes in cpu.h: CPU* first, then u64 operands and int parameters */
static V fp_call(SnxJit* j, const char* name, T ret, unsigned wide, V* operands, unsigned count) {
    gcc_jit_type* p[8];
    V args[8];
    unsigned i;
    p[0] = j->t_ptr;
    args[0] = j->c;
    for (i = 0; i < count; ++i) {
        p[i + 1] = i < wide ? j->t_u64 : j->t_int;
        args[i + 1] = operands[i];
    }
    return jit_call(j, jit_extern(j, name, ret, (int)count + 1, p), (int)count + 1, args);
}
V jit_fp_binary(SnxJit* j, V a, V b, unsigned width, unsigned op) {
    V x[4];
    x[0] = a, x[1] = b, x[2] = jit_int(j, (int)width), x[3] = jit_int(j, (int)op);
    return fp_call(j, "snx_fp_binary", j->t_u64, 2, x, 4);
}
V jit_fp_fma(SnxJit* j, V add, V a, V b, unsigned width) {
    V x[4];
    x[0] = add, x[1] = a, x[2] = b, x[3] = jit_int(j, (int)width);
    return fp_call(j, "snx_fp_fma", j->t_u64, 3, x, 4);
}
static V minmax(SnxJit* j, V a, V b, unsigned width, unsigned minimum, unsigned numeric) {
    V x[5];
    x[0] = a, x[1] = b, x[2] = jit_int(j, (int)width), x[3] = jit_int(j, (int)minimum), x[4] = jit_int(j, (int)numeric);
    return fp_call(j, "snx_fp_minmax", j->t_u64, 2, x, 5);
}
static V unary(SnxJit* j, V bits, unsigned width, unsigned op) {
    V x[3];
    x[0] = bits, x[1] = jit_int(j, (int)width), x[2] = jit_int(j, (int)op);
    return fp_call(j, "snx_fp_unary", j->t_u64, 1, x, 3);
}
static V convert(SnxJit* j, V bits, unsigned from, unsigned to, V rm) {
    V x[4];
    x[0] = bits, x[1] = jit_int(j, (int)from), x[2] = jit_int(j, (int)to), x[3] = rm;
    return fp_call(j, "snx_fp_convert", j->t_u64, 1, x, 4);
}
static V round_to(SnxJit* j, V bits, unsigned width, V rm, unsigned exact) {
    V x[4];
    x[0] = bits, x[1] = jit_int(j, (int)width), x[2] = rm, x[3] = jit_int(j, (int)exact);
    return fp_call(j, "snx_fp_round", j->t_u64, 1, x, 4);
}
static V compare(SnxJit* j, V a, V b, unsigned width, V signal) {
    V x[4];
    x[0] = a, x[1] = b, x[2] = jit_int(j, (int)width), x[3] = signal;
    return fp_call(j, "snx_fp_compare", j->t_u32, 2, x, 4);
}
static V to_int(SnxJit* j, V bits, unsigned fp, unsigned integer, unsigned uns, unsigned fbits, unsigned rm) {
    V x[6];
    x[0] = bits, x[1] = jit_int(j, (int)fp), x[2] = jit_int(j, (int)integer), x[3] = jit_int(j, (int)uns),
    x[4] = jit_int(j, (int)fbits), x[5] = jit_int(j, (int)rm);
    return fp_call(j, "snx_fp_to_int", j->t_u64, 1, x, 6);
}
static V from_int(SnxJit* j, V bits, unsigned integer, unsigned fp, unsigned uns, unsigned fbits) {
    V x[5];
    x[0] = bits, x[1] = jit_int(j, (int)integer), x[2] = jit_int(j, (int)fp), x[3] = jit_int(j, (int)uns),
    x[4] = jit_int(j, (int)fbits);
    return fp_call(j, "snx_fp_from_int", j->t_u64, 1, x, 5);
}
/* FPRM: (int)((c->fpcr >> 22) & 3) */
static V fprm(SnxJit* j) {
    return jit_cast(j, AND(SHR(jit_ld(j, offsetof(CPU, fpcr), j->t_u32), K32(22)), K32(3)), j->t_int);
}

/* SETSB/SETDB/SETH(d,e): t=(uN)(e); c->v[d].d[1]=0; c->v[d].d[0]=t; */
static void set_bits(SnxJit* j, unsigned d, unsigned bits, V value) {
    V t = jit_val(j, jit_cast(j, value, jit_uint(j, bits)));
    jit_set_vd(j, d, 1, K64(0));
    jit_set_vd(j, d, 0, U64(t));
}
/* c->v[n].s[0], .d[0] or .h[0] */
static V operand(SnxJit* j, unsigned n, unsigned width) {
    return jit_ve(j, n, width == 64 ? 3 : width == 32 ? 2 : 1, 0);
}
static V sign(SnxJit* j, V x, unsigned width) {
    T t = jit_uint(j, width);
    return XOR(jit_cast(j, x, t), jit_k(j, t, (SnxU64)1 << (width - 1)));
}
/* cond_ok(c,cond) for a constant condition (jit_a64.c has the same) */
static V condition(SnxJit* j, unsigned cond) {
    V zero = K32(0), r, n, z, cf, vf;
    unsigned k = cond >> 1;
    if (k == 7) return jit_true(j);
    n = jit_ld(j, offsetof(CPU, n), j->t_u32);
    z = jit_ld(j, offsetof(CPU, z), j->t_u32);
    cf = jit_ld(j, offsetof(CPU, cf), j->t_u32);
    vf = jit_ld(j, offsetof(CPU, vf), j->t_u32);
    switch (k) {
        case 0: r = NE(z, zero); break;
        case 1: r = NE(cf, zero); break;
        case 2: r = NE(n, zero); break;
        case 3: r = NE(vf, zero); break;
        case 4: r = jit_land(j, NE(cf, zero), EQ(z, zero)); break;
        case 5: r = EQ(n, vf); break;
        default: r = jit_land(j, EQ(z, zero), EQ(n, vf)); break;
    }
    return jit_val(j, cond & 1 ? jit_lnot(j, r) : r);
}
/* set_nzcv(c,f) */
static void nzcv_from(SnxJit* j, V f) {
    V one = K32(1);
    f = jit_val(j, U32(f));
    jit_st(j, offsetof(CPU, n), j->t_u32, AND(SHR(f, K32(3)), one));
    jit_st(j, offsetof(CPU, z), j->t_u32, AND(SHR(f, K32(2)), one));
    jit_st(j, offsetof(CPU, cf), j->t_u32, AND(SHR(f, one), one));
    jit_st(j, offsetof(CPU, vf), j->t_u32, AND(f, one));
}

static int fp2(SnxJit* j, SnxU32 w, size_t index) {
    unsigned op = index & 15, width = index >> 4 ? 64 : 32, rd = F(0, 5);
    V a = operand(j, F(5, 5), width), b = operand(j, F(16, 5), width), r;
    static const unsigned binary_ops[4] = {2, 3, 0, 1}; /* MUL, DIV, ADD, SUB */
    if (op < 4 || op == 8) {                            /* MUL, DIV, ADD, SUB, FNMUL: inline when exact */
        static const unsigned fast_ops[4] = {SNX_JIT_FP_MUL, SNX_JIT_FP_DIV, SNX_JIT_FP_ADD, SNX_JIT_FP_SUB};
        gcc_jit_block *slow, *join;
        SnxJitFpFast f;
        jit_fp_fast(j, op == 8 ? SNX_JIT_FP_MUL : fast_ops[op], width, 1, &a, &b, NULL, &f);
        join = jit_fp_branch(j, f.ok, &slow);
        set_bits(j, rd, width, op == 8 ? sign(j, f.value[0], width) : f.value[0]);
        jit_fp_else(j, join, slow, f.inexact);
        if (op == 8)
            r = sign(j, jit_fp_binary(j, a, b, width, 2), width);
        else
            r = jit_fp_binary(j, a, b, width, binary_ops[op]);
        set_bits(j, rd, width, r);
        jit_fp_join(j, join);
        return 1;
    }
    if (op >= 8) return 0;
    set_bits(j, rd, width, minmax(j, a, b, width, op & 1, (op >> 1) & 1));
    return 1;
}

static int fp1(SnxJit* j, SnxU32 w, size_t index) {
    unsigned op = index & 63, type = (unsigned)(index >> 6), rd = F(0, 5), rn = F(5, 5);
    unsigned width = type == 0 ? 32 : type == 1 ? 64 : 16;
    V a = operand(j, rn, width), r;
    if (type == 3) { /* FCVT from half */
        if (op != 4 && op != 5) return 0;
        set_bits(j, rd, op == 4 ? 32 : 64, convert(j, a, 16, op == 4 ? 32 : 64, fprm(j)));
        return 1;
    }
    if (type > 1) return 0;
    switch (op) {
        case 0: r = a; break;
        case 1: r = AND(a, jit_k(j, jit_uint(j, width), ~((SnxU64)1 << (width - 1)))); break;
        case 2: r = sign(j, a, width); break;
        case 3: { /* SNX_FP_SQRT, inline when exact */
            gcc_jit_block *slow, *join;
            SnxJitFpFast f;
            jit_fp_fast(j, SNX_JIT_FP_SQRT, width, 1, &a, NULL, NULL, &f);
            join = jit_fp_branch(j, f.ok, &slow);
            set_bits(j, rd, width, f.value[0]);
            jit_fp_else(j, join, slow, f.inexact);
            set_bits(j, rd, width, unary(j, a, width, 0));
            jit_fp_join(j, join);
            return 1;
        }
        case 4:
        case 5:
        case 7: {
            unsigned to = op == 7 ? 16 : op == 4 ? 32 : 64;
            if (to == width) return 0;
            if (to != 16) {
                /* single <-> double: widening is exact for normal values and zeros; narrowing takes the usual
                   bounds, inexact when widening back differs */
                Shape from, into;
                V f, converted, ok, inexact;
                gcc_jit_block *slow, *join;
                shape(j, width, &from);
                shape(j, to, &into);
                f = jit_val(j, to_float(j, &from, a));
                converted = jit_val(j, jit_cast(j, f, into.ft));
                if (to == 64) {
                    ok = jit_lor(j, exponent_in(j, &from, a, 1, from.emax - 1),
                                 EQ(SHL(jit_cast(j, a, from.it), kint(j, &from, 1)), kint(j, &from, 0)));
                    inexact = NULL;
                } else {
                    ok = jit_land(j, jit_land(j, rounds_to_nearest(j), exponent_in(j, &from, a, 1, from.emax - 1)),
                                  exponent_in(j, &into, to_bits(j, &into, converted), 2, into.emax - 1));
                    inexact = NE(jit_cast(j, converted, from.ft), f);
                }
                join = jit_fp_branch(j, jit_val(j, ok), &slow);
                set_bits(j, rd, to, to_bits(j, &into, converted));
                jit_fp_else(j, join, slow, inexact);
                set_bits(j, rd, to, convert(j, a, width, to, fprm(j)));
                jit_fp_join(j, join);
                return 1;
            }
            set_bits(j, rd, to, convert(j, a, width, to, fprm(j)));
            return 1;
        }
        case 8:
        case 9:
        case 10:
        case 11:
        case 12: r = round_to(j, a, width, jit_int(j, (int)op - 8), 0); break;
        case 14: r = round_to(j, a, width, fprm(j), 1); break;
        case 15: r = round_to(j, a, width, fprm(j), 0); break;
        default: return 0;
    }
    set_bits(j, rd, width, r);
    return 1;
}

/* FCMP without a NaN, and without a denormal when FZ is set (flushing it sets IDC): the host comparison gives the
   flags, and no FPSR bit changes */
static V compare_fast(SnxJit* j, V a, V b, unsigned width, V* ok) {
    Shape s;
    V fa, fb, nan, flush, nzcv;
    shape(j, width, &s);
    fa = jit_val(j, to_float(j, &s, a));
    fb = jit_val(j, to_float(j, &s, b));
    nan = jit_lor(j, NE(fa, fa), NE(fb, fb));
    flush = jit_land(
        j, flush_to_zero(j),
        jit_lor(j, jit_lnot(j, exponent_in(j, &s, a, 1, 0xFFFF)), jit_lnot(j, exponent_in(j, &s, b, 1, 0xFFFF))));
    *ok = jit_val(j, jit_lnot(j, jit_lor(j, nan, flush)));
    nzcv = jit_sel(j, LT(fa, fb), K32(8), K32(2));
    return jit_sel(j, EQ(fa, fb), K32(6), nzcv);
}

static void fcmp_flags(SnxJit* j, V a, V b, unsigned width, V signal) {
    V ok, fast = compare_fast(j, a, b, width, &ok);
    gcc_jit_block *slow, *join = jit_fp_branch(j, ok, &slow);
    nzcv_from(j, fast);
    jit_fp_else(j, join, slow, NULL);
    nzcv_from(j, compare(j, a, b, width, signal));
    jit_fp_join(j, join);
}

static void fcmp(SnxJit* j, SnxU32 w, size_t index) {
    unsigned width = index >> 2 ? 64 : 32;
    V b = index & 1 ? jit_k(j, jit_uint(j, width), 0) : operand(j, F(16, 5), width), a = operand(j, F(5, 5), width);
    fcmp_flags(j, a, b, width, jit_int(j, (int)((index >> 1) & 1)));
}

static void fccmp(SnxJit* j, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    gcc_jit_block *taken = jit_block(j), *flags = jit_block(j), *done = jit_block(j);
    jit_branch(j, condition(j, F(12, 4)), taken, flags);
    jit_enter(j, taken);
    fcmp_flags(j, operand(j, F(5, 5), width), operand(j, F(16, 5), width), width, jit_int(j, (int)F(4, 1)));
    jit_goto(j, done);
    jit_enter(j, flags);
    nzcv_from(j, K32(F(0, 4)));
    jit_goto(j, done);
    jit_enter(j, done);
}

static void fcsel(SnxJit* j, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    V ok = condition(j, F(12, 4));
    set_bits(j, F(0, 5), width, jit_sel(j, ok, operand(j, F(5, 5), width), operand(j, F(16, 5), width)));
}

/* vfp_imm32/vfp_imm64(imm8), evaluated here */
static SnxU64 vfp_imm(unsigned i, int wide) {
    SnxU64 b6 = (i >> 6) & 1, e, sign = i >> 7;
    e = ((b6 ^ 1) << (wide ? 10 : 7)) | ((b6 ? (wide ? 0xFF : 0x1F) : 0) << 2) | ((i >> 4) & 3);
    return (sign << (wide ? 63 : 31)) | (e << (wide ? 52 : 23)) | ((SnxU64)(i & 15) << (wide ? 48 : 19));
}

static void fmov_imm(SnxJit* j, SnxU32 w, size_t index) {
    set_bits(j, F(0, 5), index ? 64 : 32, K64(vfp_imm(F(13, 8), index != 0)));
}

static void fp3(SnxJit* j, SnxU32 w, size_t index) {
    unsigned o0 = index & 1, o1 = (index >> 1) & 1, width = index >> 2 ? 64 : 32;
    V a = operand(j, F(10, 5), width), n = operand(j, F(5, 5), width), m = operand(j, F(16, 5), width);
    /* FMADD, FMSUB (-n), FNMADD (-a, -n), FNMSUB (-a) */
    gcc_jit_block *slow, *join;
    SnxJitFpFast f;
    if (o1) a = jit_val(j, sign(j, a, width));
    if (o0 != o1) n = jit_val(j, sign(j, n, width));
    jit_fp_fast(j, SNX_JIT_FP_FMA, width, 1, &n, &m, &a, &f);
    join = jit_fp_branch(j, f.ok, &slow);
    set_bits(j, F(0, 5), width, f.value[0]);
    jit_fp_else(j, join, slow, f.inexact);
    set_bits(j, F(0, 5), width, jit_fp_fma(j, a, n, m, width));
    jit_fp_join(j, join);
}

/* FCVT[NPMZA][SU] to a 32/64-bit integer: round with the instruction's mode, convert, inexact when rounding changed
   the value. Fast for finite values in range (no IOC), denormals only without FZ (flushing sets IDC) */
static V to_int_fast(SnxJit* j, V bits, unsigned width, unsigned integer, unsigned uns, unsigned rm, V* ok,
                     V* inexact) {
    static const char* const rounding[5] = {"__builtin_roundeven", "__builtin_ceil", "__builtin_floor", NULL,
                                            "__builtin_round"};
    Shape s;
    V f, rounded, limit, low, result, e, mantissa, in_range;
    T it = uns ? jit_uint(j, integer) : jit_sint(j, integer);
    double top = integer == 64 ? 9223372036854775808.0 : 2147483648.0;
    shape(j, width, &s);
    f = jit_val(j, to_float(j, &s, bits));
    rounded = rm == 3 ? f : math(j, &s, rounding[rm], 1, &f); /* the conversion truncates by itself */
    e = exponent(j, &s, bits);
    mantissa = AND(jit_cast(j, bits, s.it), kint(j, &s, ((SnxU64)1 << s.mantissa) - 1));
    *ok = jit_land(
        j, NE(e, kint(j, &s, s.emax)),
        jit_lor(j, jit_lnot(j, flush_to_zero(j)), jit_lor(j, NE(e, kint(j, &s, 0)), EQ(mantissa, kint(j, &s, 0)))));
    if (uns) {
        limit = kfloat(j, &s, top * 2);
        low = kfloat(j, &s, 0.0); /* -0 compares equal: FCVTPU(-0.3) is 0, inexact */
    } else {
        limit = kfloat(j, &s, top);
        low = kfloat(j, &s, -top);
    }
    in_range = jit_land(j, GE(rounded, low), LT(rounded, limit));
    *ok = jit_val(j, jit_land(j, *ok, in_range));
    /* converted only where it is in range: out of range the C conversion is undefined, and GCC may assume it isn't */
    {
        gcc_jit_lvalue* r = jit_local(j, it);
        gcc_jit_block *convert = jit_block(j), *done = jit_block(j);
        jit_assign(j, r, jit_k(j, it, 0));
        jit_branch(j, *ok, convert, done);
        jit_enter(j, convert);
        jit_assign(j, r, jit_cast(j, rounded, it));
        jit_goto(j, done);
        jit_enter(j, done);
        result = jit_val(j, gcc_jit_lvalue_as_rvalue(r));
    }
    if (rm == 3) /* the conversion rounded: the integer, converted back (exact while it is in range) */
        rounded = jit_cast(j, result, s.ft);
    *inexact = jit_val(j, NE(rounded, f));
    return U64(jit_cast(j, result, jit_uint(j, integer))); /* the helper masks a 32-bit result to 32 bits */
}

/* FCVT_INT and FCVT_FIX: index = opcode | rmode << 3 | ftype << 5 | sf << 7 */
static int fcvt(SnxJit* j, SnxU32 w, size_t index, int fixed) {
    unsigned opcode = index & 7, rmode = (index >> 3) & 3, type = (index >> 5) & 3, sf = (unsigned)(index >> 7);
    unsigned rd = F(0, 5), rn = F(5, 5), integer = sf ? 64 : 32, width = type == 0 ? 32 : type == 1 ? 64 : 16;
    unsigned fbits = fixed ? 64 - F(10, 6) : 0;
    switch (opcode) {
        case 0:
        case 1:
        case 4:
        case 5:
            if (type > 1) return 0;
            {
                unsigned rm = opcode >= 4 ? 4 : rmode;
                V a = operand(j, rn, width), ok, inexact, fast;
                gcc_jit_block *slow, *join;
                if (fbits || (rm != 3 && !j->round)) {
                    jit_setx(j, rd, to_int(j, a, width, integer, opcode & 1, fbits, rm));
                    return 1;
                }
                fast = to_int_fast(j, a, width, integer, opcode & 1, rm, &ok, &inexact);
                join = jit_fp_branch(j, ok, &slow);
                jit_setx(j, rd, fast);
                jit_fp_else(j, join, slow, inexact);
                jit_setx(j, rd, to_int(j, a, width, integer, opcode & 1, fbits, rm));
                jit_fp_join(j, join);
            }
            return 1;
        case 2:
        case 3:
            if (type > 1) return 0;
            if (fbits) {
                set_bits(j, rd, width, from_int(j, jit_rx(j, rn), integer, width, opcode & 1, fbits));
                return 1;
            }
            {
                /* SCVTF/UCVTF: exact for magnitudes up to 2^precision; with IXC set the host's rounding suffices */
                Shape s;
                V x = jit_rx(j, rn), v, f, ok, small;
                gcc_jit_block *slow, *join;
                SnxU64 p;
                shape(j, width, &s);
                p = (SnxU64)1 << s.precision;
                if (opcode & 1) {
                    v = jit_cast(j, x, jit_uint(j, integer));
                    small = LE(U64(v), K64(p));
                } else {
                    v = jit_cast(j, x, jit_sint(j, integer));
                    small = LE(ADD(U64(jit_cast(j, v, j->t_s64)), K64(p)), K64(2 * p));
                }
                f = jit_cast(j, v, s.ft);
                ok = jit_val(j, jit_land(j, rounds_to_nearest(j), jit_lor(j, small, ixc_set(j))));
                join = jit_fp_branch(j, ok, &slow);
                set_bits(j, rd, width, to_bits(j, &s, f));
                jit_fp_else(j, join, slow, NULL);
                set_bits(j, rd, width, from_int(j, x, integer, width, opcode & 1, fbits));
                jit_fp_join(j, join);
            }
            return 1;
        case 6: /* FMOV to a general register */
            if (type == 2)
                jit_setx(j, rd, jit_vd(j, rn, 1));
            else
                jit_setx(j, rd, U64(operand(j, rn, width)));
            return 1;
        case 7: /* FMOV from a general register */
            if (type == 2)
                jit_set_vd(j, rd, 1, jit_rx(j, rn));
            else
                set_bits(j, rd, width, width == 64 ? jit_rx(j, rn) : U32(jit_rx(j, rn)));
            return 1;
    }
    return 0;
}

int snx_jit_fp_plain(SnxJit* j, SnxU32 w, SnxU64 pc, const SnxA64Pattern* pattern, size_t index) {
    const char* name = pattern->name;
    (void)pc;
    if (!strcmp(name, "FP2")) return fp2(j, w, index);
    if (!strcmp(name, "FP1")) return fp1(j, w, index);
    if (!strcmp(name, "FCMP"))
        fcmp(j, w, index);
    else if (!strcmp(name, "FCCMP"))
        fccmp(j, w, index);
    else if (!strcmp(name, "FCSEL"))
        fcsel(j, w, index);
    else if (!strcmp(name, "FMOV_IMM"))
        fmov_imm(j, w, index);
    else if (!strcmp(name, "FP3"))
        fp3(j, w, index);
    else if (!strcmp(name, "FCVT_INT"))
        return fcvt(j, w, index, 0);
    else if (!strcmp(name, "FCVT_FIX"))
        return fcvt(j, w, index, 1);
    else
        return 0;
    return 1;
}
