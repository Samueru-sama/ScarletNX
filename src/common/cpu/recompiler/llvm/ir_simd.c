/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AdvSIMD: snx_a64_fast_simd's shortcuts (a64.c), emitted inline as the C it prints, and the SIMD patterns, whose
   template hands the word to snx_simd (cpu_helpers.c, in the runtime library) */
#include "ir.h"
#include <string.h>

typedef LLVMValueRef V;
#define B ir->b

static LLVMTypeRef lane_type(SnxIr* ir, unsigned size) {
    return size == 0 ? ir->t_i8 : size == 1 ? ir->t_i16 : size == 2 ? ir->t_i32 : ir->t_i64;
}

/* lane index of 1<<size bytes in a V128 at pointer base (a register or a local) */
static V lane_pointer(SnxIr* ir, V base, unsigned size, unsigned index) {
    V offset = ir_i64(ir, (SnxU64)index << size);
    return LLVMBuildInBoundsGEP2(B, ir->t_i8, base, &offset, 1, "");
}
static V get(SnxIr* ir, V base, unsigned size, unsigned index) {
    return ir_load(ir, lane_type(ir, size), lane_pointer(ir, base, size, index));
}
static void set(SnxIr* ir, V base, unsigned size, unsigned index, V value) {
    ir_store(ir, ir_cast(ir, value, lane_type(ir, size), 0), lane_pointer(ir, base, size, index));
}
static V vreg(SnxIr* ir, unsigned n) { return ir_cpu(ir, ir_off_v(n)); }
/* V128 x = <source>: a local copy */
static V copy_of(SnxIr* ir, V source) {
    V local = ir_local(ir, ir_v128(ir));
    LLVMBuildMemCpy(B, local, 1, source, 1, ir_i64(ir, 16));
    return local;
}
static void assign(SnxIr* ir, V destination, V source) {
    LLVMBuildMemCpy(B, destination, 1, source, 1, ir_i64(ir, 16));
}

static V fp_binary(SnxIr* ir, V a, V b, unsigned width, unsigned op) {
    LLVMTypeRef p[5];
    V args[5];
    p[0] = ir->t_ptr;
    p[1] = p[2] = ir->t_i64;
    p[3] = p[4] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir_u64(ir, a);
    args[2] = ir_u64(ir, b);
    args[3] = ir_i32(ir, width);
    args[4] = ir_i32(ir, op);
    return ir_call(ir, ir_extern(ir, "snx_fp_binary", ir->t_i64, p, 5), args, 5);
}
static V fp_fma(SnxIr* ir, V add, V a, V b, unsigned width) {
    LLVMTypeRef p[5];
    V args[5];
    p[0] = ir->t_ptr;
    p[1] = p[2] = p[3] = ir->t_i64;
    p[4] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir_u64(ir, add);
    args[2] = ir_u64(ir, a);
    args[3] = ir_u64(ir, b);
    args[4] = ir_i32(ir, width);
    return ir_call(ir, ir_extern(ir, "snx_fp_fma", ir->t_i64, p, 5), args, 5);
}
/* x ^ sign bit of the lane, or x */
static V negate(SnxIr* ir, V x, unsigned sz, int on) {
    if (!on) return x;
    return LLVMBuildXor(B, x, sz ? ir_i64(ir, (SnxU64)1 << 63) : ir_i32(ir, 0x80000000UL), "");
}

int snx_ir_fast_simd(SnxIr* ir, SnxU32 w) {
    unsigned q, u, size, rd, rn, rm, op, hi, sz, lanes, i, scalar, l, h, mb, index, m, imm5, imm4, lane;
    q = (w >> 30) & 1;
    u = (w >> 29) & 1;
    size = (w >> 22) & 3;
    rd = w & 31;
    rn = (w >> 5) & 31;
    rm = (w >> 16) & 31;
    if ((w & 0x9F200400UL) == 0x0E200400UL) {
        op = (w >> 11) & 31;
        if (op == 3) {
            V a0, a1, b0, b1;
            if (!u && size == 2 && rn == rm) { /* MOV (ORR Vd,Vn,Vn) */
                ir_set_vd(ir, rd, 0, ir_vd(ir, rn, 0));
                ir_set_vd(ir, rd, 1, q ? ir_vd(ir, rn, 1) : ir_i64(ir, 0));
                return 1;
            }
            if (u && size) return 0;
            a0 = ir_vd(ir, rn, 0);
            a1 = ir_vd(ir, rn, 1);
            b0 = ir_vd(ir, rm, 0);
            b1 = ir_vd(ir, rm, 1);
            if (!u && (size & 1)) { /* &~, |~ */
                b0 = LLVMBuildNot(B, b0, "");
                b1 = LLVMBuildNot(B, b1, "");
            }
            if (u) {
                ir_set_vd(ir, rd, 0, LLVMBuildXor(B, a0, b0, ""));
                ir_set_vd(ir, rd, 1, q ? LLVMBuildXor(B, a1, b1, "") : ir_i64(ir, 0));
            } else if (size < 2) {
                ir_set_vd(ir, rd, 0, LLVMBuildAnd(B, a0, b0, ""));
                ir_set_vd(ir, rd, 1, q ? LLVMBuildAnd(B, a1, b1, "") : ir_i64(ir, 0));
            } else {
                ir_set_vd(ir, rd, 0, LLVMBuildOr(B, a0, b0, ""));
                ir_set_vd(ir, rd, 1, q ? LLVMBuildOr(B, a1, b1, "") : ir_i64(ir, 0));
            }
            return 1;
        }
        if (op >= 0x18) {
            int operation = -1, acc = 0, negated = 0;
            V a, b, r;
            sz = size & 1;
            hi = size >> 1;
            if (sz && !q) return 0;
            lanes = sz ? 2 : (q ? 4 : 2);
            if (!u && !hi && op == 0x1A)
                operation = 0; /* SNX_FP_ADD */
            else if (!u && hi && op == 0x1A)
                operation = 1; /* SNX_FP_SUB */
            else if (u && !hi && op == 0x1B)
                operation = 2; /* SNX_FP_MUL */
            else if (u && !hi && op == 0x1F)
                operation = 3; /* SNX_FP_DIV */
            else if (!u && op == 0x19) {
                acc = 1;
                negated = (int)hi;
            }
            if (operation < 0 && !acc) return 0;
            /* { V128 a=c->v[rn],b=c->v[rm],r[=c->v[rd]]; r.T[i]=...; if(!q) r.d[1]=0; c->v[rd]=r; } */
            a = copy_of(ir, vreg(ir, rn));
            b = copy_of(ir, vreg(ir, rm));
            r = acc ? copy_of(ir, vreg(ir, rd)) : ir_local(ir, ir_v128(ir));
            for (i = 0; i < lanes; ++i) {
                unsigned s = sz ? 3 : 2;
                V value;
                if (acc)
                    value = fp_fma(ir, get(ir, r, s, i), negate(ir, get(ir, a, s, i), sz, negated), get(ir, b, s, i),
                                   sz ? 64 : 32);
                else
                    value = fp_binary(ir, get(ir, a, s, i), get(ir, b, s, i), sz ? 64 : 32, (unsigned)operation);
                set(ir, r, s, i, value);
            }
            if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
            assign(ir, vreg(ir, rd), r);
            return 1;
        }
        if (op == 0x10 && (q || size < 3)) { /* ADD, SUB (vector) */
            V a, b, r;
            lanes = (q ? 16 : 8) >> size;
            a = copy_of(ir, vreg(ir, rn));
            b = copy_of(ir, vreg(ir, rm));
            r = ir_local(ir, ir_v128(ir));
            for (i = 0; i < lanes; ++i) {
                V x = get(ir, a, size, i), y = get(ir, b, size, i);
                set(ir, r, size, i, u ? LLVMBuildSub(B, x, y, "") : LLVMBuildAdd(B, x, y, ""));
            }
            if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
            assign(ir, vreg(ir, rd), r);
            return 1;
        }
        return 0;
    }
    if ((w & 0x9F000400UL) == 0x0F000000UL || (w & 0xDF000400UL) == 0x5F000000UL) { /* FMLA/FMLS/FMUL by element */
        V a, r, d0 = NULL, e;
        unsigned s;
        scalar = (w >> 28) & 1;
        op = (w >> 12) & 15;
        if (u || (op != 1 && op != 5 && op != 9) || size < 2) return 0;
        sz = size & 1;
        l = (w >> 21) & 1;
        h = (w >> 11) & 1;
        mb = (w >> 20) & 1;
        if (sz && l) return 0;
        index = sz ? h : ((h << 1) | l);
        m = (mb << 4) | ((w >> 16) & 15);
        if (!scalar && sz && !q) return 0;
        lanes = scalar ? 1 : (sz ? 2 : (q ? 4 : 2));
        s = sz ? 3 : 2;
        a = copy_of(ir, vreg(ir, rn));
        r = !scalar && op != 9 ? copy_of(ir, vreg(ir, rd)) : ir_local(ir, ir_v128(ir));
        if (scalar && op != 9) d0 = get(ir, vreg(ir, rd), s, 0);
        if (scalar || op == 9) {
            set(ir, r, 3, 0, ir_i64(ir, 0));
            set(ir, r, 3, 1, ir_i64(ir, 0));
        }
        e = get(ir, vreg(ir, m), s, index);
        for (i = 0; i < lanes; ++i) {
            V value;
            if (op == 9)
                value = fp_binary(ir, get(ir, a, s, i), e, sz ? 64 : 32, 2);
            else
                value = fp_fma(ir, scalar ? d0 : get(ir, r, s, i), negate(ir, get(ir, a, s, i), sz, op == 5), e,
                               sz ? 64 : 32);
            set(ir, r, s, i, value);
        }
        if (!q && !scalar) set(ir, r, 3, 1, ir_i64(ir, 0));
        assign(ir, vreg(ir, rd), r);
        return 1;
    }
    if ((w & 0x9FE08400UL) == 0x0E000400UL) { /* DUP, INS, UMOV */
        imm5 = (w >> 16) & 31;
        imm4 = (w >> 11) & 15;
        if (!imm5) return 0;
        sz = 0;
        while (!(imm5 & (1U << sz))) ++sz;
        if (sz > 3) return 0;
        index = imm5 >> (sz + 1);
        lane = sz;
        lanes = (q ? 16 : 8) >> sz;
        if (!u && imm4 == 7 && ((sz == 3) == !!q)) {
            if (rd != 31) ir_set_x(ir, rd, ir_u64(ir, ir_ve(ir, rn, lane, index)));
            return 1;
        }
        if (!u && imm4 == 3 && q) {
            ir_set_ve(ir, rd, lane, index, rn == 31 ? ir_i64(ir, 0) : ir_x(ir, rn));
            return 1;
        }
        if (u && q) {
            ir_set_ve(ir, rd, lane, index, ir_ve(ir, rn, lane, imm4 >> sz));
            return 1;
        }
        if (!u && (imm4 == 0 || imm4 == 1) && !(sz == 3 && !q)) {
            V r = ir_local(ir, ir_v128(ir)), e;
            if (!imm4)
                e = ir_ve(ir, rn, lane, index);
            else
                e = ir_cast(ir, rn == 31 ? ir_i64(ir, 0) : ir_x(ir, rn), lane_type(ir, lane), 0);
            for (i = 0; i < lanes; ++i) set(ir, r, lane, i, e);
            if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
            assign(ir, vreg(ir, rd), r);
            return 1;
        }
        return 0;
    }
    if ((w & 0xFFFFFFE0UL) == 0x2F00E400UL || (w & 0xFFFFFFE0UL) == 0x6F00E400UL) { /* MOVI Vd, #0 */
        ir_set_vd(ir, rd, 0, ir_i64(ir, 0));
        ir_set_vd(ir, rd, 1, ir_i64(ir, 0));
        return 1;
    }
    return 0;
}

/* SIMD_LDST, SIMD_LDST1, SIMD_DP, SIMD_DP2: if(!snx_simd(c,M,w)) c->rt->undef(c,pc,w); */
int snx_ir_simd_plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index) {
    LLVMTypeRef p[3];
    V args[3], handled;
    LLVMBasicBlockRef undefined, done;
    (void)index;
    if (strcmp(pattern->name, "SIMD_LDST") && strcmp(pattern->name, "SIMD_LDST1") && strcmp(pattern->name, "SIMD_DP") &&
        strcmp(pattern->name, "SIMD_DP2"))
        return 0;
    p[0] = ir->t_ptr;
    p[1] = ir->t_ptr;
    p[2] = ir->t_i32;
    args[0] = ir->c;
    args[1] = ir->M;
    args[2] = ir_i32(ir, word);
    handled = ir_call(ir, ir_extern(ir, "snx_simd", ir->t_i32, p, 3), args, 3);
    undefined = ir_block(ir, "simd_undef");
    done = ir_block(ir, "simd_done");
    LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, handled, ir_i32(ir, 0), ""), undefined, done);
    LLVMPositionBuilderAtEnd(B, undefined);
    ir_undef(ir, pc, word);
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
    return 1;
}
