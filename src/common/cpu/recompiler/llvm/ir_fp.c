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
        case 3: r = unary(ir, a, width, 0); break; /* SNX_FP_SQRT */
        case 4:
        case 5:
        case 7: {
            unsigned to = op == 7 ? 16 : op == 4 ? 32 : 64;
            if (to == width) return 0;
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

static void fcmp(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned width = index >> 2 ? 64 : 32;
    V b = index & 1 ? ir_i64(ir, 0) : operand(ir, F(16, 5), width);
    set_nzcv(ir, compare(ir, operand(ir, F(5, 5), width), b, width, i32c(ir, (index >> 1) & 1)));
}

static void fccmp(SnxIr* ir, SnxU32 w, size_t index) {
    unsigned width = index ? 64 : 32;
    LLVMBasicBlockRef taken = ir_block(ir, "fccmp"), flags = ir_block(ir, "nzcv"), done = ir_block(ir, "done");
    LLVMBuildCondBr(B, truth(ir, cond_ok(ir, F(12, 4))), taken, flags);
    LLVMPositionBuilderAtEnd(B, taken);
    set_nzcv(ir, compare(ir, operand(ir, F(5, 5), width), operand(ir, F(16, 5), width), width, i32c(ir, F(4, 1))));
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
    if (o1) a = sign(ir, a, width);
    if (o0 != o1) n = sign(ir, n, width);
    set_bits(ir, F(0, 5), width, fma3(ir, a, n, m, width));
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
            ir_setx(ir, rd,
                    to_int(ir, operand(ir, rn, width), width, integer, opcode & 1, fbits, opcode >= 4 ? 4 : rmode));
            return 1;
        case 2:
        case 3:
            if (type > 1) return 0;
            set_bits(ir, rd, width, from_int(ir, ir_rx(ir, rn), integer, width, opcode & 1, fbits));
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
