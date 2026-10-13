/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Scalar floating point: the FP patterns of a64_templates.inc. Their C calls the integer-only FP of cpu_helpers.c
   (snx_fp_*), in the runtime library, and writes the result with SETSB/SETDB/SETH. */
#include "ir.h"
#include <string.h>

typedef LLVMValueRef V;
#define B        ir->b
#define F(lo, n) ((w >> (lo)) & ((1UL << (n)) - 1))

static V i32c(SnxIr* ir, SnxU32 v) { return ir_i32(ir, v); }

/* ---- fast paths ---- */

typedef struct {
    LLVMTypeRef it, ft; /* lane types: iN and its float */
    unsigned lanes, mantissa, emax, precision;
} Shape;

static void shape(SnxIr* ir, unsigned width, unsigned lanes, Shape* s) {
    LLVMTypeRef i = width == 64 ? ir->t_i64 : ir->t_i32;
    LLVMTypeRef f = width == 64 ? LLVMDoubleTypeInContext(ir->ctx) : LLVMFloatTypeInContext(ir->ctx);
    s->lanes = lanes;
    s->it = lanes > 1 ? LLVMVectorType(i, lanes) : i;
    s->ft = lanes > 1 ? LLVMVectorType(f, lanes) : f;
    s->mantissa = width == 64 ? 52 : 23;
    s->emax = width == 64 ? 0x7FF : 0xFF;
    s->precision = s->mantissa + 1;
}
static V splat(const Shape* s, V scalar) {
    V values[4];
    unsigned i;
    if (s->lanes == 1) return scalar;
    for (i = 0; i < s->lanes; ++i) values[i] = scalar;
    return LLVMConstVector(values, s->lanes);
}
static V kint(SnxIr* ir, const Shape* s, SnxU64 v) {
    (void)ir;
    return splat(s, LLVMConstInt(s->lanes > 1 ? LLVMGetElementType(s->it) : s->it, v, 0));
}
static V kfloat(SnxIr* ir, const Shape* s, double v) {
    (void)ir;
    return splat(s, LLVMConstReal(s->lanes > 1 ? LLVMGetElementType(s->ft) : s->ft, v));
}
/* every lane / some lane of an i1 (vector) */
static V all(SnxIr* ir, const Shape* s, V c) {
    return s->lanes > 1 ? ir_intrinsic(ir, "llvm.vector.reduce.and", LLVMTypeOf(c), &c, 1) : c;
}
static V any(SnxIr* ir, const Shape* s, V c) {
    return s->lanes > 1 ? ir_intrinsic(ir, "llvm.vector.reduce.or", LLVMTypeOf(c), &c, 1) : c;
}
static V exponent(SnxIr* ir, const Shape* s, V bits) {
    return LLVMBuildAnd(B, LLVMBuildLShr(B, bits, kint(ir, s, s->mantissa), ""), kint(ir, s, s->emax), "");
}
/* lo <= exponent <= hi, every lane */
static V exponent_in(SnxIr* ir, const Shape* s, V bits, unsigned lo, unsigned hi) {
    V e = exponent(ir, s, bits);
    return all(ir, s, LLVMBuildICmp(B, LLVMIntULE, LLVMBuildSub(B, e, kint(ir, s, lo), ""), kint(ir, s, hi - lo), ""));
}
static V rounds_to_nearest(SnxIr* ir) {
    V fpcr = ir_cpu_load(ir, ir->t_i32, ir_off_fpcr());
    return LLVMBuildICmp(B, LLVMIntEQ, LLVMBuildAnd(B, fpcr, i32c(ir, 3u << 22), ""), i32c(ir, 0), "");
}
static V ixc_set(SnxIr* ir) {
    V fpsr = ir_cpu_load(ir, ir->t_i32, ir_off_fpcr() + 4);
    return LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, fpsr, i32c(ir, 1u << 4), ""), i32c(ir, 0), "");
}
/* the host fuses multiply-add in hardware (x86 FMA3 from CPUID; AArch64 always): llvm.fma is then one instruction,
   never a call into a C library the module doesn't link */
static int host_fma(SnxIr* ir) {
    if (ir->features->x86) return ir->features->fma;
    return !strncmp(LLVMGetTarget(ir->mod), "aarch64", 7);
}
static V fp_intrinsic(SnxIr* ir, const char* name, const Shape* s, V* args, unsigned count) {
    return ir_intrinsic(ir, name, s->ft, args, count);
}
static V nonzero(SnxIr* ir, const Shape* s, V f) {
    return any(ir, s, LLVMBuildFCmp(B, LLVMRealUNE, f, kfloat(ir, s, 0.0), ""));
}

void ir_fp_fast(SnxIr* ir, unsigned op, unsigned width, unsigned lanes, V a, V b, V add, SnxFpFast* out) {
    Shape s;
    V fa, fb, r, ok, exact_ok, args[3], ixc;
    unsigned p, top;
    shape(ir, width, lanes, &s);
    p = s.precision;
    top = s.emax - 1;
    fa = LLVMBuildBitCast(B, a, s.ft, "");
    fb = b ? LLVMBuildBitCast(B, b, s.ft, "") : NULL;
    ok = LLVMBuildAnd(B, rounds_to_nearest(ir), exponent_in(ir, &s, a, 1, top), "");
    if (b) ok = LLVMBuildAnd(B, ok, exponent_in(ir, &s, b, 1, top), "");
    ixc = ixc_set(ir);
    exact_ok = ixc;
    out->inexact = LLVMConstInt(ir->t_i1, 0, 0);
    switch (op) {
        case SNX_IR_FP_ADD:
        case SNX_IR_FP_SUB: {
            /* TwoSum: the rounding error of a sum is a float, and exactly da+db */
            V y = op == SNX_IR_FP_SUB ? LLVMBuildFNeg(B, fb, "") : fb, bp, ap;
            r = LLVMBuildFAdd(B, fa, y, "");
            bp = LLVMBuildFSub(B, r, fa, "");
            ap = LLVMBuildFSub(B, r, bp, "");
            out->inexact =
                nonzero(ir, &s, LLVMBuildFAdd(B, LLVMBuildFSub(B, fa, ap, ""), LLVMBuildFSub(B, y, bp, ""), ""));
            exact_ok = LLVMConstInt(ir->t_i1, 1, 0);
            break;
        }
        case SNX_IR_FP_MUL:
            r = LLVMBuildFMul(B, fa, fb, "");
            if (host_fma(ir)) { /* a*b - r, exact while the result is at least 2^(emin+p) */
                args[0] = fa, args[1] = fb, args[2] = LLVMBuildFNeg(B, r, "");
                out->inexact = nonzero(ir, &s, fp_intrinsic(ir, "llvm.fma", &s, args, 3));
                exact_ok =
                    LLVMBuildOr(B, ixc, exponent_in(ir, &s, LLVMBuildBitCast(B, r, s.it, ""), p + 2, top - 1), "");
            }
            break;
        case SNX_IR_FP_DIV:
            r = LLVMBuildFDiv(B, fa, fb, "");
            if (host_fma(ir)) { /* a - r*b, exact under the same bounds on a and r */
                args[0] = LLVMBuildFNeg(B, r, ""), args[1] = fb, args[2] = fa;
                out->inexact = nonzero(ir, &s, fp_intrinsic(ir, "llvm.fma", &s, args, 3));
                exact_ok =
                    LLVMBuildOr(B, ixc,
                                LLVMBuildAnd(B, exponent_in(ir, &s, LLVMBuildBitCast(B, r, s.it, ""), p + 2, top - 1),
                                             exponent_in(ir, &s, a, p + 2, top - 1), ""),
                                "");
            }
            break;
        case SNX_IR_FP_SQRT: {
            V positive = all(ir, &s, LLVMBuildICmp(B, LLVMIntSGE, a, kint(ir, &s, 0), ""));
            ok = LLVMBuildAnd(B, ok, positive, "");
            r = fp_intrinsic(ir, "llvm.sqrt", &s, &fa, 1);
            if (host_fma(ir)) { /* a - r*r */
                args[0] = LLVMBuildFNeg(B, r, ""), args[1] = r, args[2] = fa;
                out->inexact = nonzero(ir, &s, fp_intrinsic(ir, "llvm.fma", &s, args, 3));
                exact_ok = LLVMBuildOr(B, ixc, exponent_in(ir, &s, a, p + 2, top - 1), "");
            }
            break;
        }
        default: /* FMA: add + a*b, fused; whether it is exact isn't cheap to know, so only once IXC is set */
            args[0] = fa, args[1] = fb, args[2] = LLVMBuildBitCast(B, add, s.ft, "");
            ok = LLVMBuildAnd(B, ok, exponent_in(ir, &s, add, 1, top), "");
            r = host_fma(ir) ? fp_intrinsic(ir, "llvm.fma", &s, args, 3) : NULL;
            if (!r) { /* no hardware FMA: never fast */
                r = LLVMBuildFAdd(B, args[2], LLVMBuildFMul(B, fa, fb, ""), "");
                exact_ok = LLVMConstInt(ir->t_i1, 0, 0);
            }
            break;
    }
    out->value = LLVMBuildBitCast(B, r, s.it, "");
    /* no zero, denormal or smallest-binade result (tininess is detected before rounding), no overflow */
    ok = LLVMBuildAnd(B, ok, exponent_in(ir, &s, out->value, 2, top), "");
    out->ok = LLVMBuildAnd(B, ok, exact_ok, "");
}

LLVMBasicBlockRef ir_fp_branch(SnxIr* ir, V ok, LLVMBasicBlockRef* slow) {
    LLVMBasicBlockRef fast = ir_block(ir, "fp_fast"), join = ir_block(ir, "fp_join");
    LLVMMetadataRef md[3];
    V br;
    *slow = ir_block(ir, "fp_slow");
    br = LLVMBuildCondBr(B, ok, fast, *slow);
    md[0] = LLVMMDStringInContext2(ir->ctx, "branch_weights", 14);
    md[1] = LLVMValueAsMetadata(ir_i32(ir, 2000));
    md[2] = LLVMValueAsMetadata(ir_i32(ir, 1));
    LLVMSetMetadata(br, LLVMGetMDKindIDInContext(ir->ctx, "prof", 4),
                    LLVMMetadataAsValue(ir->ctx, LLVMMDNodeInContext2(ir->ctx, md, 3)));
    LLVMPositionBuilderAtEnd(B, fast);
    return join;
}
void ir_fp_else(SnxIr* ir, LLVMBasicBlockRef join, LLVMBasicBlockRef slow, V inexact) {
    if (!LLVMIsConstant(inexact) || LLVMConstIntGetZExtValue(inexact)) {
        size_t off = ir_off_fpcr() + 4; /* fpsr |= IXC */
        V fpsr = ir_cpu_load(ir, ir->t_i32, off);
        ir_cpu_store(ir, LLVMBuildOr(B, fpsr, LLVMBuildShl(B, ir_bool32(ir, inexact), i32c(ir, 4), ""), ""), off);
    }
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, slow);
}
void ir_fp_join(SnxIr* ir, LLVMBasicBlockRef join) {
    LLVMBuildBr(B, join);
    LLVMPositionBuilderAtEnd(B, join);
}

/* the helpers' prototypes in cpu.h: CPU* first, then u64 operands and int parameters */
static V fp_call(SnxIr* ir, const char* name, LLVMTypeRef ret, unsigned wide, V* operands, unsigned count) {
    LLVMTypeRef p[8];
    V args[8];
    unsigned i;
    p[0] = ir->t_ptr;
    args[0] = ir->c;
    for (i = 0; i < count; ++i) {
        p[i + 1] = i < wide ? ir->t_i64 : ir->t_i32;
        args[i + 1] = i < wide ? ir_u64(ir, operands[i]) : operands[i];
    }
    return ir_call(ir, ir_extern(ir, name, ret, p, count + 1), args, count + 1);
}
static V binary(SnxIr* ir, V a, V b, unsigned width, unsigned op) {
    V x[4];
    x[0] = a, x[1] = b, x[2] = i32c(ir, width), x[3] = i32c(ir, op);
    return fp_call(ir, "snx_fp_binary", ir->t_i64, 2, x, 4);
}
static V minmax(SnxIr* ir, V a, V b, unsigned width, unsigned minimum, unsigned numeric) {
    V x[5];
    x[0] = a, x[1] = b, x[2] = i32c(ir, width), x[3] = i32c(ir, minimum), x[4] = i32c(ir, numeric);
    return fp_call(ir, "snx_fp_minmax", ir->t_i64, 2, x, 5);
}
static V fma3(SnxIr* ir, V add, V a, V b, unsigned width) {
    V x[4];
    x[0] = add, x[1] = a, x[2] = b, x[3] = i32c(ir, width);
    return fp_call(ir, "snx_fp_fma", ir->t_i64, 3, x, 4);
}
static V unary(SnxIr* ir, V bits, unsigned width, unsigned op) {
    V x[3];
    x[0] = bits, x[1] = i32c(ir, width), x[2] = i32c(ir, op);
    return fp_call(ir, "snx_fp_unary", ir->t_i64, 1, x, 3);
}
static V convert(SnxIr* ir, V bits, unsigned from, unsigned to, V rm) {
    V x[4];
    x[0] = bits, x[1] = i32c(ir, from), x[2] = i32c(ir, to), x[3] = rm;
    return fp_call(ir, "snx_fp_convert", ir->t_i64, 1, x, 4);
}
static V round_to(SnxIr* ir, V bits, unsigned width, V rm, unsigned exact) {
    V x[4];
    x[0] = bits, x[1] = i32c(ir, width), x[2] = rm, x[3] = i32c(ir, exact);
    return fp_call(ir, "snx_fp_round", ir->t_i64, 1, x, 4);
}
static V compare(SnxIr* ir, V a, V b, unsigned width, V signal) {
    V x[4];
    x[0] = a, x[1] = b, x[2] = i32c(ir, width), x[3] = signal;
    return fp_call(ir, "snx_fp_compare", ir->t_i32, 2, x, 4);
}
static V to_int(SnxIr* ir, V bits, unsigned fp, unsigned integer, unsigned uns, unsigned fbits, unsigned rm) {
    V x[6];
    x[0] = bits, x[1] = i32c(ir, fp), x[2] = i32c(ir, integer), x[3] = i32c(ir, uns), x[4] = i32c(ir, fbits),
    x[5] = i32c(ir, rm);
    return fp_call(ir, "snx_fp_to_int", ir->t_i64, 1, x, 6);
}
static V from_int(SnxIr* ir, V bits, unsigned integer, unsigned fp, unsigned uns, unsigned fbits) {
    V x[5];
    x[0] = bits, x[1] = i32c(ir, integer), x[2] = i32c(ir, fp), x[3] = i32c(ir, uns), x[4] = i32c(ir, fbits);
    return fp_call(ir, "snx_fp_from_int", ir->t_i64, 1, x, 5);
}
/* FPRM: (int)((c->fpcr >> 22) & 3) */
static V fprm(SnxIr* ir) {
    V fpcr = ir_cpu_load(ir, ir->t_i32, ir_off_fpcr());
    return LLVMBuildAnd(B, LLVMBuildLShr(B, fpcr, i32c(ir, 22), ""), i32c(ir, 3), "");
}

/* SETSB/SETDB/SETH(d,e): t=(uN)(e); c->v[d].d[1]=0; c->v[d].d[0]=t; */
static void set_bits(SnxIr* ir, unsigned d, unsigned bits, V value) {
    V t = ir_cast(ir, value, LLVMIntTypeInContext(ir->ctx, bits), 0);
    ir_set_vd(ir, d, 1, ir_i64(ir, 0));
    ir_set_vd(ir, d, 0, ir_u64(ir, t));
}

/* c->v[n].s[0], .d[0] or .h[0] for ftype 0, 1, 3 */
static V operand(SnxIr* ir, unsigned n, unsigned width) {
    return ir_ve(ir, n, width == 64 ? 3 : width == 32 ? 2 : 1, 0);
}
static V sign(SnxIr* ir, V x, unsigned width) {
    return LLVMBuildXor(B, x, width == 64 ? ir_i64(ir, (SnxU64)1 << 63) : i32c(ir, 0x80000000UL), "");
}
static V truth(SnxIr* ir, V v) { return LLVMBuildICmp(B, LLVMIntNE, v, i32c(ir, 0), ""); }
static V cond_ok(SnxIr* ir, unsigned cond) {
    V args[2];
    args[0] = ir->c;
    args[1] = i32c(ir, cond);
    return ir_call_helper(ir, SNX_IR_COND_OK, args, 2);
}
static void set_nzcv(SnxIr* ir, V flags) {
    V args[2];
    args[0] = ir->c;
    args[1] = flags;
    ir_call_helper(ir, SNX_IR_SET_NZCV, args, 2);
}

static int fp2(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned op = index & 15, width = index >> 4 ? 64 : 32, rd = F(0, 5);
    V a = operand(ir, F(5, 5), width), b = operand(ir, F(16, 5), width), r;
    static const unsigned binary_ops[4] = {2, 3, 0, 1}; /* MUL, DIV, ADD, SUB */
    if (op < 4 || op == 8) {                            /* MUL, DIV, ADD, SUB, FNMUL: inline when exact */
        static const unsigned fast_ops[4] = {SNX_IR_FP_MUL, SNX_IR_FP_DIV, SNX_IR_FP_ADD, SNX_IR_FP_SUB};
        LLVMBasicBlockRef slow, join;
        SnxFpFast f;
        ir_fp_fast(ir, op == 8 ? SNX_IR_FP_MUL : fast_ops[op], width, 1, a, b, NULL, &f);
        join = ir_fp_branch(ir, f.ok, &slow);
        set_bits(ir, rd, width, op == 8 ? sign(ir, f.value, width) : f.value);
        ir_fp_else(ir, join, slow, f.inexact);
        if (op == 8)
            r = sign(ir, ir_cast(ir, binary(ir, a, b, width, 2), width == 64 ? ir->t_i64 : ir->t_i32, 0), width);
        else
            r = binary(ir, a, b, width, binary_ops[op]);
        set_bits(ir, rd, width, r);
        ir_fp_join(ir, join);
        return 1;
    }
    if (op < 4)
        r = binary(ir, a, b, width, binary_ops[op]);
    else if (op < 8)
        r = minmax(ir, a, b, width, op & 1, (op >> 1) & 1);
    else if (op == 8) /* FNMUL */
        r = sign(ir, ir_cast(ir, binary(ir, a, b, width, 2), width == 64 ? ir->t_i64 : ir->t_i32, 0), width);
    else
        return 0;
    set_bits(ir, rd, width, r);
    return 1;
}

static int fp1(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned op = index & 63, type = (unsigned)(index >> 6), rd = F(0, 5), rn = F(5, 5);
    unsigned width = type == 0 ? 32 : type == 1 ? 64 : 16;
    V a = operand(ir, rn, width), r;
    if (type == 3) { /* FCVT from half */
        if (op != 4 && op != 5) return 0;
        set_bits(ir, rd, op == 4 ? 32 : 64, convert(ir, a, 16, op == 4 ? 32 : 64, fprm(ir)));
        return 1;
    }
    if (type > 1) return 0;
    switch (op) {
        case 0: r = a; break;
        case 1:
            r = LLVMBuildAnd(B, a, width == 64 ? ir_i64(ir, ~((SnxU64)1 << 63)) : i32c(ir, 0x7FFFFFFFUL), "");
            break;
        case 2: r = sign(ir, a, width); break;
        case 3: { /* SNX_FP_SQRT, inline when exact */
            LLVMBasicBlockRef slow, join;
            SnxFpFast f;
            ir_fp_fast(ir, SNX_IR_FP_SQRT, width, 1, a, NULL, NULL, &f);
            join = ir_fp_branch(ir, f.ok, &slow);
            set_bits(ir, rd, width, f.value);
            ir_fp_else(ir, join, slow, f.inexact);
            set_bits(ir, rd, width, unary(ir, a, width, 0));
            ir_fp_join(ir, join);
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
                V f, r, ok, inexact;
                LLVMBasicBlockRef slow, join;
                shape(ir, width, 1, &from);
                shape(ir, to, 1, &into);
                f = LLVMBuildBitCast(B, a, from.ft, "");
                if (to == 64) {
                    r = LLVMBuildFPExt(B, f, into.ft, "");
                    ok = LLVMBuildOr(
                        B, exponent_in(ir, &from, a, 1, from.emax - 1),
                        LLVMBuildICmp(B, LLVMIntEQ, LLVMBuildShl(B, a, kint(ir, &from, 1), ""), kint(ir, &from, 0), ""),
                        "");
                    inexact = LLVMConstInt(ir->t_i1, 0, 0);
                } else {
                    r = LLVMBuildFPTrunc(B, f, into.ft, "");
                    ok = LLVMBuildAnd(
                        B, LLVMBuildAnd(B, rounds_to_nearest(ir), exponent_in(ir, &from, a, 1, from.emax - 1), ""),
                        exponent_in(ir, &into, LLVMBuildBitCast(B, r, into.it, ""), 2, into.emax - 1), "");
                    inexact = LLVMBuildFCmp(B, LLVMRealUNE, LLVMBuildFPExt(B, r, from.ft, ""), f, "");
                }
                join = ir_fp_branch(ir, ok, &slow);
                set_bits(ir, rd, to, LLVMBuildBitCast(B, r, into.it, ""));
                ir_fp_else(ir, join, slow, inexact);
                set_bits(ir, rd, to, convert(ir, a, width, to, fprm(ir)));
                ir_fp_join(ir, join);
                return 1;
            }
            set_bits(ir, rd, to, convert(ir, a, width, to, fprm(ir)));
            return 1;
        }
        case 8:
        case 9:
        case 10:
        case 11:
        case 12: r = round_to(ir, a, width, i32c(ir, op - 8), 0); break;
        case 14: r = round_to(ir, a, width, fprm(ir), 1); break;
        case 15: r = round_to(ir, a, width, fprm(ir), 0); break;
        default: return 0;
    }
    set_bits(ir, rd, width, r);
    return 1;
}

/* FCMP without a NaN, and without a denormal when FZ is set (flushing it sets IDC): the host comparison gives the
   flags, and no FPSR bit changes */
static V compare_fast(SnxIr* ir, V a, V b, unsigned width, V* ok) {
    Shape s;
    V fa, fb, nan_a, nan_b, flush, nzcv;
    shape(ir, width, 1, &s);
    fa = LLVMBuildBitCast(B, ir_cast(ir, a, s.it, 0), s.ft, "");
    fb = LLVMBuildBitCast(B, ir_cast(ir, b, s.it, 0), s.ft, "");
    nan_a = LLVMBuildFCmp(B, LLVMRealUNO, fa, fa, "");
    nan_b = LLVMBuildFCmp(B, LLVMRealUNO, fb, fb, "");
    flush = LLVMBuildAnd(
        B,
        LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, ir_cpu_load(ir, ir->t_i32, ir_off_fpcr()), i32c(ir, 1u << 24), ""),
                      i32c(ir, 0), ""),
        LLVMBuildOr(B, LLVMBuildNot(B, exponent_in(ir, &s, ir_cast(ir, a, s.it, 0), 1, 0xFFFF), ""),
                    LLVMBuildNot(B, exponent_in(ir, &s, ir_cast(ir, b, s.it, 0), 1, 0xFFFF), ""), ""),
        "");
    *ok = LLVMBuildNot(B, LLVMBuildOr(B, LLVMBuildOr(B, nan_a, nan_b, ""), flush, ""), "");
    nzcv = LLVMBuildSelect(B, LLVMBuildFCmp(B, LLVMRealOLT, fa, fb, ""), i32c(ir, 8), i32c(ir, 2), "");
    return LLVMBuildSelect(B, LLVMBuildFCmp(B, LLVMRealOEQ, fa, fb, ""), i32c(ir, 6), nzcv, "");
}

static void fcmp(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned width = index >> 2 ? 64 : 32;
    V b = index & 1 ? ir_i64(ir, 0) : operand(ir, F(16, 5), width), a = operand(ir, F(5, 5), width), ok, fast;
    LLVMBasicBlockRef slow, join;
    fast = compare_fast(ir, a, b, width, &ok);
    join = ir_fp_branch(ir, ok, &slow);
    set_nzcv(ir, fast);
    ir_fp_else(ir, join, slow, LLVMConstInt(ir->t_i1, 0, 0));
    set_nzcv(ir, compare(ir, a, b, width, i32c(ir, (index >> 1) & 1)));
    ir_fp_join(ir, join);
}

static void fccmp(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    LLVMBasicBlockRef taken = ir_block(ir, "fccmp"), flags = ir_block(ir, "nzcv"), done = ir_block(ir, "done");
    LLVMBuildCondBr(B, truth(ir, cond_ok(ir, F(12, 4))), taken, flags);
    LLVMPositionBuilderAtEnd(B, taken);
    {
        V a = operand(ir, F(5, 5), width), b = operand(ir, F(16, 5), width), ok, fast;
        LLVMBasicBlockRef slow, join;
        fast = compare_fast(ir, a, b, width, &ok);
        join = ir_fp_branch(ir, ok, &slow);
        set_nzcv(ir, fast);
        ir_fp_else(ir, join, slow, LLVMConstInt(ir->t_i1, 0, 0));
        set_nzcv(ir, compare(ir, a, b, width, i32c(ir, F(4, 1))));
        ir_fp_join(ir, join);
    }
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, flags);
    set_nzcv(ir, i32c(ir, F(0, 4)));
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
}

static void fcsel(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    V ok = truth(ir, cond_ok(ir, F(12, 4)));
    set_bits(ir, F(0, 5), width, LLVMBuildSelect(B, ok, operand(ir, F(5, 5), width), operand(ir, F(16, 5), width), ""));
}

static void fmov_imm(SnxIr* ir, SnxU32 w, size_t index) {
    V imm = i32c(ir, F(13, 8));
    set_bits(ir, F(0, 5), index ? 64 : 32, ir_call_helper(ir, index ? SNX_IR_VFP_IMM64 : SNX_IR_VFP_IMM32, &imm, 1));
}

static void fp3(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned o0 = index & 1, o1 = (index >> 1) & 1, width = index >> 2 ? 64 : 32;
    V a = operand(ir, F(10, 5), width), n = operand(ir, F(5, 5), width), m = operand(ir, F(16, 5), width);
    /* FMADD, FMSUB (-n), FNMADD (-a, -n), FNMSUB (-a) */
    LLVMBasicBlockRef slow, join;
    SnxFpFast f;
    if (o1) a = sign(ir, a, width);
    if (o0 != o1) n = sign(ir, n, width);
    ir_fp_fast(ir, SNX_IR_FP_FMA, width, 1, n, m, a, &f);
    join = ir_fp_branch(ir, f.ok, &slow);
    set_bits(ir, F(0, 5), width, f.value);
    ir_fp_else(ir, join, slow, f.inexact);
    set_bits(ir, F(0, 5), width, fma3(ir, a, n, m, width));
    ir_fp_join(ir, join);
}

/* the host rounds to an integral value in one instruction (x86 SSE4.1 ROUNDSD, part of the CPUID sse4.2 gate;
   AArch64 FRINT*): llvm.floor and friends never become C library calls */
static int host_round(SnxIr* ir) {
    if (ir->features->x86) return ir->features->sse42;
    return !strncmp(LLVMGetTarget(ir->mod), "aarch64", 7);
}

/* FCVT[NPMZA][SU] to a 32/64-bit integer: round with the instruction's mode, convert, inexact when rounding changed
   the value. Fast for finite values in range (no IOC), denormals only without FZ (flushing sets IDC) */
static V to_int_fast(SnxIr* ir, V bits, unsigned width, unsigned integer, unsigned uns, unsigned rm, V* ok,
                     V* inexact) {
    static const char* const rounding[5] = {"llvm.roundeven", "llvm.ceil", "llvm.floor", "llvm.trunc", "llvm.round"};
    Shape s;
    V f, rounded, fz, limit, low, result, e, mantissa;
    LLVMTypeRef it = LLVMIntTypeInContext(ir->ctx, integer);
    double top = integer == 64 ? 9223372036854775808.0 : 2147483648.0;
    shape(ir, width, 1, &s);
    f = LLVMBuildBitCast(B, bits, s.ft, "");
    rounded = rm == 3 ? f : fp_intrinsic(ir, rounding[rm], &s, &f, 1); /* FPToSI truncates by itself */
    e = exponent(ir, &s, bits);
    mantissa = LLVMBuildAnd(B, bits, kint(ir, &s, (((SnxU64)1 << s.mantissa) - 1)), "");
    fz = LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, ir_cpu_load(ir, ir->t_i32, ir_off_fpcr()), i32c(ir, 1u << 24), ""),
                       i32c(ir, 0), "");
    *ok = LLVMBuildAnd(B, LLVMBuildICmp(B, LLVMIntNE, e, kint(ir, &s, s.emax), ""),
                       LLVMBuildOr(B, LLVMBuildNot(B, fz, ""),
                                   LLVMBuildOr(B, LLVMBuildICmp(B, LLVMIntNE, e, kint(ir, &s, 0), ""),
                                               LLVMBuildICmp(B, LLVMIntEQ, mantissa, kint(ir, &s, 0), ""), ""),
                                   ""),
                       "");
    if (uns) {
        limit = kfloat(ir, &s, top * 2);
        low = kfloat(ir, &s, 0.0); /* -0 compares equal: FCVTPU(-0.3) is 0, inexact */
        result = LLVMBuildFPToUI(B, rounded, it, "");
    } else {
        limit = kfloat(ir, &s, top);
        low = kfloat(ir, &s, -top);
        result = LLVMBuildFPToSI(B, rounded, it, "");
    }
    *ok = LLVMBuildAnd(B, *ok,
                       LLVMBuildAnd(B, LLVMBuildFCmp(B, LLVMRealOGE, rounded, low, ""),
                                    LLVMBuildFCmp(B, LLVMRealOLT, rounded, limit, ""), ""),
                       "");
    if (rm == 3) /* FPToSI rounded: the integer, converted back (exact while it is in range) */
        rounded = uns ? LLVMBuildUIToFP(B, result, s.ft, "") : LLVMBuildSIToFP(B, result, s.ft, "");
    *inexact = LLVMBuildFCmp(B, LLVMRealUNE, rounded, f, "");
    return ir_u64(ir, result); /* the helper masks a 32-bit result to 32 bits */
}

/* FCVT_INT and FCVT_FIX: index = opcode | rmode << 3 | ftype << 5 | sf << 7 */
static int fcvt(SnxIr* ir, SnxU32 w, size_t index, int fixed) {
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
                V a = operand(ir, rn, width), ok, inexact, fast;
                LLVMBasicBlockRef slow, join;
                if (fbits || (rm != 3 && !host_round(ir))) {
                    ir_setx(ir, rd, to_int(ir, a, width, integer, opcode & 1, fbits, rm));
                    return 1;
                }
                fast = to_int_fast(ir, a, width, integer, opcode & 1, rm, &ok, &inexact);
                join = ir_fp_branch(ir, ok, &slow);
                ir_setx(ir, rd, fast);
                ir_fp_else(ir, join, slow, inexact);
                ir_setx(ir, rd, to_int(ir, a, width, integer, opcode & 1, fbits, rm));
                ir_fp_join(ir, join);
            }
            return 1;
        case 2:
        case 3:
            if (type > 1) return 0;
            if (fbits) {
                set_bits(ir, rd, width, from_int(ir, ir_rx(ir, rn), integer, width, opcode & 1, fbits));
                return 1;
            }
            {
                /* SCVTF/UCVTF: exact for magnitudes up to 2^precision; with IXC set the host's rounding suffices */
                Shape s;
                V x = ir_rx(ir, rn), v, f, ok, small;
                LLVMBasicBlockRef slow, join;
                LLVMTypeRef it = LLVMIntTypeInContext(ir->ctx, integer);
                SnxU64 p;
                shape(ir, width, 1, &s);
                p = (SnxU64)1 << s.precision;
                v = LLVMBuildTrunc(B, x, it, "");
                f = opcode & 1 ? LLVMBuildUIToFP(B, v, s.ft, "") : LLVMBuildSIToFP(B, v, s.ft, "");
                if (opcode & 1)
                    small = LLVMBuildICmp(B, LLVMIntULE, ir_u64(ir, v), ir_i64(ir, p), "");
                else
                    small = LLVMBuildICmp(B, LLVMIntULE, LLVMBuildAdd(B, ir_s64(ir, v), ir_i64(ir, p), ""),
                                          ir_i64(ir, 2 * p), "");
                ok = LLVMBuildAnd(B, rounds_to_nearest(ir), LLVMBuildOr(B, small, ixc_set(ir), ""), "");
                join = ir_fp_branch(ir, ok, &slow);
                set_bits(ir, rd, width, LLVMBuildBitCast(B, f, s.it, ""));
                ir_fp_else(ir, join, slow, LLVMConstInt(ir->t_i1, 0, 0));
                set_bits(ir, rd, width, from_int(ir, x, integer, width, opcode & 1, fbits));
                ir_fp_join(ir, join);
            }
            return 1;
        case 6: /* FMOV to a general register */
            if (type == 2)
                ir_setx(ir, rd, ir_vd(ir, rn, 1));
            else
                ir_setx(ir, rd, ir_u64(ir, operand(ir, rn, width)));
            return 1;
        case 7: /* FMOV from a general register */
            if (type == 2)
                ir_set_vd(ir, rd, 1, ir_rx(ir, rn));
            else
                set_bits(ir, rd, width, width == 64 ? ir_rx(ir, rn) : ir_u32(ir, ir_rx(ir, rn)));
            return 1;
    }
    return 0;
}

int snx_ir_fp_plain(SnxIr* ir, SnxU32 w, SnxU64 pc, const SnxA64Pattern* pattern, size_t index) {
    const char* name = pattern->name;
    (void)pc;
    if (!strcmp(name, "FP2")) return fp2(ir, w, index);
    if (!strcmp(name, "FP1")) return fp1(ir, w, index);
    if (!strcmp(name, "FCMP"))
        fcmp(ir, w, index);
    else if (!strcmp(name, "FCCMP"))
        fccmp(ir, w, index);
    else if (!strcmp(name, "FCSEL"))
        fcsel(ir, w, index);
    else if (!strcmp(name, "FMOV_IMM"))
        fmov_imm(ir, w, index);
    else if (!strcmp(name, "FP3"))
        fp3(ir, w, index);
    else if (!strcmp(name, "FCVT_INT"))
        return fcvt(ir, w, index, 0);
    else if (!strcmp(name, "FCVT_FIX"))
        return fcvt(ir, w, index, 1);
    else
        return 0;
    return 1;
}
