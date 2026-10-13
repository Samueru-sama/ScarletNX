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

/* lanes 0..n-1 of 1<<size bytes at base as one value: iN for one lane, <n x iN> otherwise */
static LLVMTypeRef lanes_type(SnxIr* ir, unsigned size, unsigned n) {
    return n == 1 ? lane_type(ir, size) : LLVMVectorType(lane_type(ir, size), n);
}
static V get_lanes(SnxIr* ir, V base, unsigned size, unsigned n) {
    V load = ir_load(ir, lanes_type(ir, size, n), base);
    LLVMSetAlignment(load, 1);
    return load;
}
static void set_lanes(SnxIr* ir, V base, V value) {
    V store = LLVMBuildStore(B, value, base);
    LLVMSetAlignment(store, 1);
}
static V broadcast(SnxIr* ir, V scalar, unsigned n) {
    V v = LLVMGetUndef(LLVMVectorType(LLVMTypeOf(scalar), n));
    unsigned i;
    if (n == 1) return scalar;
    for (i = 0; i < n; ++i) v = LLVMBuildInsertElement(B, v, scalar, ir_i32(ir, i), "");
    return v;
}
static V negate_lanes(SnxIr* ir, V x, unsigned sz, unsigned n) {
    return LLVMBuildXor(B, x, broadcast(ir, sz ? ir_i64(ir, (SnxU64)1 << 63) : ir_i32(ir, 0x80000000UL), n), "");
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
            {
                /* all lanes at once on the host's vector unit when every lane is exact (ir_fp_fast) */
                unsigned s = sz ? 3 : 2;
                V va = get_lanes(ir, a, s, lanes), vb = get_lanes(ir, b, s, lanes);
                LLVMBasicBlockRef slow, join;
                SnxFpFast f;
                if (acc)
                    ir_fp_fast(ir, SNX_IR_FP_FMA, sz ? 64 : 32, lanes, negated ? negate_lanes(ir, va, sz, lanes) : va,
                               vb, get_lanes(ir, r, s, lanes), &f);
                else
                    ir_fp_fast(ir, (unsigned)operation, sz ? 64 : 32, lanes, va, vb, NULL, &f);
                join = ir_fp_branch(ir, f.ok, &slow);
                set_lanes(ir, r, f.value);
                if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
                assign(ir, vreg(ir, rd), r);
                ir_fp_else(ir, join, slow, f.inexact);
                for (i = 0; i < lanes; ++i) {
                    V value;
                    if (acc)
                        value = fp_fma(ir, get(ir, r, s, i), negate(ir, get(ir, a, s, i), sz, negated),
                                       get(ir, b, s, i), sz ? 64 : 32);
                    else
                        value = fp_binary(ir, get(ir, a, s, i), get(ir, b, s, i), sz ? 64 : 32, (unsigned)operation);
                    set(ir, r, s, i, value);
                }
                if (!q) set(ir, r, 3, 1, ir_i64(ir, 0));
                assign(ir, vreg(ir, rd), r);
                ir_fp_join(ir, join);
                return 1;
            }
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
        {
            V va = get_lanes(ir, a, s, lanes), ve = broadcast(ir, e, lanes);
            LLVMBasicBlockRef slow, join;
            SnxFpFast f;
            if (op == 9)
                ir_fp_fast(ir, SNX_IR_FP_MUL, sz ? 64 : 32, lanes, va, ve, NULL, &f);
            else
                ir_fp_fast(ir, SNX_IR_FP_FMA, sz ? 64 : 32, lanes, op == 5 ? negate_lanes(ir, va, sz, lanes) : va, ve,
                           scalar ? d0 : get_lanes(ir, r, s, lanes), &f);
            join = ir_fp_branch(ir, f.ok, &slow);
            set_lanes(ir, r, f.value);
            if (!q && !scalar) set(ir, r, 3, 1, ir_i64(ir, 0));
            assign(ir, vreg(ir, rd), r);
            ir_fp_else(ir, join, slow, f.inexact);
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
            ir_fp_join(ir, join);
            return 1;
        }
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
    ir_unlikely(ir, LLVMBuildCondBr(B, LLVMBuildICmp(B, LLVMIntEQ, handled, ir_i32(ir, 0), ""), undefined, done));
    LLVMPositionBuilderAtEnd(B, undefined);
    ir_undef(ir, pc, word);
    LLVMBuildBr(B, done);
    LLVMPositionBuilderAtEnd(B, done);
    return 1;
}

/* ---- beyond snx_a64_fast_simd: encodings snx_simd handles, emitted inline with its semantics ---- */

static V v128_type_load(SnxIr* ir, V base) { /* the 16 bytes at base as <16 x i8> */
    V load = ir_load(ir, LLVMVectorType(ir->t_i8, 16), base);
    LLVMSetAlignment(load, 1);
    return load;
}
static void zero_high(SnxIr* ir, unsigned n) { ir_set_vd(ir, n, 1, ir_i64(ir, 0)); }

/* LD1/ST1 {Vt..Vt+n-1}: simd_ldst stores each element in turn; for consecutive registers that is one copy per
   register. Post-index adds the size, or Xm */
static int ld1_multiple(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, op = (w >> 12) & 15, regs, bytes = q ? 16 : 8, r;
    V address;
    regs = op == 7 ? 1 : op == 10 ? 2 : op == 6 ? 3 : op == 2 ? 4 : 0;
    if (!regs) return 0;
    address = ir_x(ir, rn);
    for (r = 0; r < regs; ++r) {
        V a = LLVMBuildAdd(B, address, ir_i64(ir, (SnxU64)r * bytes), "");
        if (l)
            set_lanes(ir, vreg(ir, (rt + r) & 31), ir_rd_bytes(ir, a, bytes));
        else
            ir_wr_bytes(ir, a, get_lanes(ir, vreg(ir, (rt + r) & 31), 0, bytes));
    }
    if (l && !q)
        for (r = 0; r < regs; ++r) zero_high(ir, (rt + r) & 31);
    if (post)
        ir_set_x(ir, rn, LLVMBuildAdd(B, address, rm == 31 ? ir_i64(ir, (SnxU64)regs * bytes) : ir_x(ir, rm), ""));
    return 1;
}

/* LD1R and LD1/ST1 of one lane (one register) */
static int ld1_single(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, r = (w >> 21) & 1, op = (w >> 13) & 7, s = (w >> 12) & 1, size = (w >> 10) & 3;
    unsigned sc = op >> 1, esz, idx, i, n, bytes;
    V address;
    if ((op & 1) || r) return 0; /* LD2-4 and friends */
    address = ir_x(ir, rn);
    if (sc == 3) { /* LD1R: the element in every lane, the rest zero */
        V value, local;
        if (!l || s) return 0;
        n = (q ? 128u : 64u) >> (3 + size);
        value = ir_rd(ir, address, 8u << size);
        local = ir_local(ir, ir_v128(ir));
        set(ir, local, 3, 0, ir_i64(ir, 0));
        set(ir, local, 3, 1, ir_i64(ir, 0));
        for (i = 0; i < n; ++i) set(ir, local, size, i, value);
        assign(ir, vreg(ir, rt), local);
        bytes = 1u << size;
    } else {
        if (sc == 0) {
            esz = 0;
            idx = (q << 3) | (s << 2) | size;
        } else if (sc == 1) {
            if (size & 1) return 0;
            esz = 1;
            idx = (q << 2) | (s << 1) | (size >> 1);
        } else {
            if (size & 2) return 0;
            if (size & 1) {
                if (s) return 0;
                esz = 3;
                idx = q;
            } else {
                esz = 2;
                idx = (q << 1) | s;
            }
        }
        if (l)
            ir_set_ve(ir, rt, esz, idx, ir_rd(ir, address, 8u << esz));
        else
            ir_wr(ir, address, 8u << esz, ir_ve(ir, rt, esz, idx));
        bytes = 1u << esz;
    }
    if (post) ir_set_x(ir, rn, LLVMBuildAdd(B, address, rm == 31 ? ir_i64(ir, bytes) : ir_x(ir, rm), ""));
    return 1;
}

/* store <lanes> into Vd with the upper half cleared when Q is 0 (simd_perm and EXT write r from zero) */
static void write_lanes(SnxIr* ir, unsigned rd, V value, unsigned q) {
    V local = ir_local(ir, ir_v128(ir));
    set(ir, local, 3, 1, ir_i64(ir, 0));
    set_lanes(ir, local, value);
    if (!q) set(ir, local, 3, 1, ir_i64(ir, 0));
    assign(ir, vreg(ir, rd), local);
}
static V shuffle(SnxIr* ir, V a, V b, const unsigned* indices, unsigned n) {
    V mask[16];
    unsigned i;
    for (i = 0; i < n; ++i) mask[i] = ir_i32(ir, indices[i]);
    return LLVMBuildShuffleVector(B, a, b, LLVMConstVector(mask, n), "");
}

/* UZP1/2, TRN1/2, ZIP1/2: simd_perm's element formulas, as one shuffle */
static int permute(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 7, rn = (w >> 5) & 31,
             rd = w & 31, n, h, part, i, indices[16];
    if ((size == 3 && !q) || !(op & 3)) return 0;
    n = (q ? 128u : 64u) >> (3 + size);
    h = n / 2;
    part = (op >> 2) & 1;
    for (i = 0; i < n; ++i) {
        unsigned k;
        if ((op & 3) == 1) /* UZP: k < n ? a[k] : b[k-n] */
            k = 2 * i + part;
        else if ((op & 3) == 2) /* TRN */
            k = (i & 1) ? n + i / 2 * 2 + part : i / 2 * 2 + part;
        else /* ZIP */
            k = (i & 1) ? n + part * h + i / 2 : part * h + i / 2;
        indices[i] = k;
    }
    write_lanes(ir, rd,
                shuffle(ir, get_lanes(ir, vreg(ir, rn), size, n), get_lanes(ir, vreg(ir, rm), size, n), indices, n), q);
    return 1;
}

/* EXT: bytes imm4.. of Vn:Vm */
static int extract(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, imm4 = (w >> 11) & 15, rn = (w >> 5) & 31, rd = w & 31,
             len = q ? 16 : 8, i, indices[16];
    if (!q && (imm4 & 8)) return 0;
    for (i = 0; i < len; ++i) {
        unsigned source = imm4 + i;
        indices[i] = source < len ? source : 16 + (source - len);
    }
    write_lanes(ir, rd, shuffle(ir, v128_type_load(ir, vreg(ir, rn)), v128_type_load(ir, vreg(ir, rm)), indices, len),
                q);
    return 1;
}

/* ---- GFNI: GF2P8AFFINEQB as a per-byte bit matrix ----
   (wunkolo, "gf2p8affineqb: int8 shifting", 2020.) Byte 7-i of the 64-bit matrix builds result bit i, its bit j
   selecting source bit j: the identity is 0x0102040810204080. Shifting the identity right by 8n bytes is a logical
   shift left by n, left by 8n a logical shift right; an arithmetic shift right also ORs n bytes of 0x80 back in,
   replicating bit 7. x86 has no 8-bit shifts, so each of these replaces a shift-and-mask sequence. */
#define GF_IDENTITY ((SnxU64)0x01020408UL << 32 | 0x10204080UL)
static SnxU64 gf_shl(unsigned n) { return n >= 8 ? 0 : GF_IDENTITY >> (8 * n); }
static SnxU64 gf_lshr(unsigned n) { return n >= 8 ? 0 : GF_IDENTITY << (8 * n); }
static SnxU64 gf_ashr(unsigned n) {
    SnxU64 sign = (SnxU64)0x80808080UL << 32 | 0x80808080UL;
    if (n > 7) n = 7;
    return n ? (GF_IDENTITY << (8 * n)) | (~(~(SnxU64)0 << (8 * n)) & sign) : GF_IDENTITY;
}
/* result bit 0 = source bit k, the rest 0 */
static SnxU64 gf_bit(unsigned k) { return ((SnxU64)1 << k) << 56; }

/* the host's GF2P8AFFINEQB applies, from CPUID (snx_ir_host): an x86-64 target with GFNI */
static int host_gfni(SnxIr* ir) { return ir->features->x86 && ir->features->gfni; }

/* GF2P8AFFINEQB x, matrix, 0 on 16 bytes */
static V gf_affine(SnxIr* ir, V x, SnxU64 matrix) {
    V bytes[16], args[3];
    unsigned i;
    for (i = 0; i < 16; ++i) bytes[i] = ir_i8(ir, (unsigned)(matrix >> (8 * (i & 7))));
    args[0] = x;
    args[1] = LLVMConstVector(bytes, 16);
    args[2] = ir_i8(ir, 0);
    return ir_intrinsic(ir, "llvm.x86.vgf2p8affineqb.128", NULL, args, 3);
}

/* the low n lanes of a <16 x i8> */
static V low_lanes(SnxIr* ir, V x, unsigned n) {
    unsigned indices[16], i;
    if (n == 16) return x;
    for (i = 0; i < n; ++i) indices[i] = i;
    return shuffle(ir, x, x, indices, n);
}

/* CNT, NOT, RBIT (byte lanes): llvm.ctpop and llvm.bitreverse become VPOPCNTB and GF2P8AFFINEQB on x86 hosts with
   AVX512_BITALG and GFNI, a shuffle-table sequence otherwise, CNT and RBIT on AArch64 */
static int byte_misc(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, n = q ? 16 : 8;
    V a, r;
    LLVMTypeRef t;
    if (op != 5 || !(u ? size < 2 : size == 0)) return 0;
    t = LLVMVectorType(ir->t_i8, n);
    a = get_lanes(ir, vreg(ir, rn), 0, n);
    if (!u)
        r = ir_intrinsic(ir, "llvm.ctpop", t, &a, 1);
    else if (!size)
        r = LLVMBuildNot(B, a, "");
    else if (host_gfni(ir)) /* wunkolo, "gf2p8affineqb: Bit reversal": the matrix 0x8040201008040201 */
        r = low_lanes(ir, gf_affine(ir, v128_type_load(ir, vreg(ir, rn)), ((SnxU64)0x80402010UL << 32) | 0x08040201UL),
                      n);
    else
        r = ir_intrinsic(ir, "llvm.bitreverse", t, &a, 1);
    write_lanes(ir, rd, r, q);
    return 1;
}

/* v_expand_imm (helpers/simd.c), evaluated here */
static SnxU64 expand_imm(unsigned op, unsigned cmode, unsigned imm8) {
    SnxU64 i = imm8, r = 0;
    unsigned k;
    switch (cmode >> 1) {
        case 0: r = i | (i << 32); break;
        case 1: r = (i << 8) | (i << 40); break;
        case 2: r = (i << 16) | (i << 48); break;
        case 3: r = (i << 24) | (i << 56); break;
        case 4:
            r = i | (i << 16);
            r |= r << 32;
            break;
        case 5:
            r = (i << 8) | (i << 24);
            r |= r << 32;
            break;
        case 6:
            r = !(cmode & 1) ? (i << 8) | 0xFF : (i << 16) | 0xFFFF;
            r |= r << 32;
            break;
        default:
            if (!(cmode & 1) && !op) {
                for (k = 0; k < 8; k++) r |= i << (k * 8);
            } else if (!(cmode & 1) && op) {
                for (k = 0; k < 8; k++)
                    if ((i >> k) & 1) r |= (SnxU64)0xFF << (k * 8);
            } else if ((cmode & 1) && !op) {
                SnxU32 b =
                    (SnxU32)((i & 0x80) << 24) | ((i & 0x40) ? 0x3E000000u : 0x40000000u) | ((SnxU32)(i & 0x3F) << 19);
                r = b | ((SnxU64)b << 32);
            } else {
                r = ((i & 0x80) << 56) | ((i & 0x40) ? ((SnxU64)0x3FC00000UL << 32) : ((SnxU64)0x40000000UL << 32)) |
                    ((i & 0x3F) << 48);
            }
            break;
    }
    return r;
}

/* MOVI, MVNI, ORR, BIC (vector, immediate): simd_modimm with the immediate a constant */
static int modified_immediate(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, op = (w >> 29) & 1, cmode = (w >> 12) & 15, rd = w & 31, o2 = (w >> 11) & 1;
    unsigned imm8 = (((w >> 16) & 7) << 5) | ((w >> 5) & 31);
    SnxU64 imm;
    if (o2 || (cmode == 15 && op && !q)) return 0;
    imm = expand_imm(op, cmode, imm8);
    if (cmode < 12 && (cmode & 1)) { /* ORR, BIC */
        V k = ir_i64(ir, op ? ~imm : imm);
        if (!op) {
            ir_set_vd(ir, rd, 0, LLVMBuildOr(B, ir_vd(ir, rd, 0), k, ""));
            ir_set_vd(ir, rd, 1, q ? LLVMBuildOr(B, ir_vd(ir, rd, 1), k, "") : ir_i64(ir, 0));
        } else {
            ir_set_vd(ir, rd, 0, LLVMBuildAnd(B, ir_vd(ir, rd, 0), k, ""));
            ir_set_vd(ir, rd, 1, q ? LLVMBuildAnd(B, ir_vd(ir, rd, 1), k, "") : ir_i64(ir, 0));
        }
        return 1;
    }
    if (op && cmode != 14 && cmode != 15) imm = ~imm; /* MVNI */
    ir_set_vd(ir, rd, 0, ir_i64(ir, imm));
    ir_set_vd(ir, rd, 1, ir_i64(ir, q ? imm : 0));
    return 1;
}

/* AESE/AESD/AESMC/AESIMC with AES-NI (x86, CPUID aes). Arm's AESE is ShiftRows(SubBytes(d^k)), x86's AESENCLAST
   ShiftRows(SubBytes(s))^rk: AESENCLAST(d^k, 0). MixColumns alone is AESENC(AESDECLAST(x, 0), 0), the inverse
   steps cancelling */
static int aes(SnxIr* ir, SnxU32 w) {
    unsigned op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31;
    LLVMTypeRef t = LLVMVectorType(ir->t_i64, 2);
    V args[2], zero = LLVMConstNull(t), x, r;
    if (!ir->features->x86 || !ir->features->aes || op < 4 || op > 7) return 0;
    x = ir_load(ir, t, vreg(ir, rn));
    LLVMSetAlignment(x, 8);
    if (op < 6) {
        V d = ir_load(ir, t, vreg(ir, rd));
        LLVMSetAlignment(d, 8);
        args[0] = LLVMBuildXor(B, d, x, "");
        args[1] = zero;
        r = ir_intrinsic(ir, op == 4 ? "llvm.x86.aesni.aesenclast" : "llvm.x86.aesni.aesdeclast", NULL, args, 2);
    } else if (op == 6) {
        args[0] = x;
        args[1] = zero;
        args[0] = ir_intrinsic(ir, "llvm.x86.aesni.aesdeclast", NULL, args, 2);
        r = ir_intrinsic(ir, "llvm.x86.aesni.aesenc", NULL, args, 2);
    } else
        r = ir_intrinsic(ir, "llvm.x86.aesni.aesimc", NULL, &x, 1);
    LLVMSetAlignment(LLVMBuildStore(B, r, vreg(ir, rd)), 8);
    return 1;
}

/* PMULL/PMULL2 Vd.1Q (64x64 carry-less) with PCLMULQDQ (x86, CPUID pclmulqdq) */
static int pmull_1q(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31;
    LLVMTypeRef t = LLVMVectorType(ir->t_i64, 2);
    V args[3], r;
    if (!ir->features->x86 || !ir->features->pclmul) return 0;
    args[0] = ir_load(ir, t, vreg(ir, rn));
    args[1] = ir_load(ir, t, vreg(ir, rm));
    LLVMSetAlignment(args[0], 8);
    LLVMSetAlignment(args[1], 8);
    args[2] = ir_i8(ir, q ? 0x11 : 0x00);
    r = ir_intrinsic(ir, "llvm.x86.pclmulqdq", NULL, args, 3);
    LLVMSetAlignment(LLVMBuildStore(B, r, vreg(ir, rd)), 8);
    return 1;
}

/* ---- vpternlog: any function of three bit vectors in one instruction (AVX512F+VL, from CPUID) ----
   (wunkolo, "vpternlog: Signed Saturation", 2025.) The 8-bit table is the function itself applied to the constants
   A=0xF0, B=0xCC, C=0xAA: each bit position of those is one combination of the three inputs */
enum { TERN_A = 0xF0, TERN_B = 0xCC, TERN_C = 0xAA };
static int host_ternlog(SnxIr* ir) { return ir->features->x86 && ir->features->avx512vl; }
/* VPTERNLOG{D,Q} on 128 bits: lanes of 32 or 64 bits (only the masking granularity differs) */
static V ternlog(SnxIr* ir, V a, V b, V c, unsigned table, unsigned bits) {
    V args[4];
    LLVMTypeRef t = LLVMVectorType(bits == 64 ? ir->t_i64 : ir->t_i32, bits == 64 ? 2 : 4);
    args[0] = LLVMBuildBitCast(B, a, t, "");
    args[1] = LLVMBuildBitCast(B, b, t, "");
    args[2] = LLVMBuildBitCast(B, c, t, "");
    args[3] = ir_i32(ir, table & 0xFF);
    return ir_intrinsic(ir, bits == 64 ? "llvm.x86.avx512.pternlog.q.128" : "llvm.x86.avx512.pternlog.d.128", NULL,
                        args, 4);
}

/* FPSR.QC (bit 27) |= any lane of an i1 vector */
static void set_qc(SnxIr* ir, V lanes) {
    V any = ir_intrinsic(ir, "llvm.vector.reduce.or", LLVMTypeOf(lanes), &lanes, 1);
    size_t off = ir_off_fpcr() + 4;
    V fpsr = ir_cpu_load(ir, ir->t_i32, off);
    ir_cpu_store(ir, LLVMBuildOr(B, fpsr, LLVMBuildShl(B, ir_bool32(ir, any), ir_i32(ir, 27), ""), ""), off);
}

/* SQADD, UQADD, SQSUB, UQSUB (vector): saturating, QC when a lane saturates (sqadd_ and friends). For 32/64-bit
   signed lanes on AVX-512 hosts, wunkolo's vpternlog test: a lane overflowed when the inputs' sign bits agree and the
   sum's differs (add, 0x42) or the inputs' differ and the difference's differs from the first (sub, 0x18); the
   saturated value is the result's sign spread and its top bit flipped */
static V saturating(SnxIr* ir, unsigned u, unsigned sub, unsigned eb, unsigned n, V a, V b) {
    static const char* const names[4] = {"llvm.sadd.sat", "llvm.uadd.sat", "llvm.ssub.sat", "llvm.usub.sat"};
    LLVMTypeRef t = LLVMTypeOf(a);
    V wrap = sub ? LLVMBuildSub(B, a, b, "") : LLVMBuildAdd(B, a, b, ""), args[2], r;
    if (!u && eb >= 32 && n * eb == 128 && host_ternlog(ir)) {
        unsigned table = sub ? (TERN_A ^ TERN_B) & (TERN_A ^ TERN_C) : ~(TERN_A ^ TERN_B) & (TERN_A ^ TERN_C);
        V check = LLVMBuildBitCast(B, ternlog(ir, a, b, wrap, table, eb), t, "");
        V overflow = LLVMBuildICmp(B, LLVMIntSLT, check, LLVMConstNull(t), "");
        V spread = LLVMBuildAShr(B, wrap, broadcast(ir, LLVMConstInt(LLVMGetElementType(t), eb - 1, 0), n), "");
        V value = LLVMBuildXor(B, spread,
                               broadcast(ir, LLVMConstInt(LLVMGetElementType(t), (SnxU64)1 << (eb - 1), 0), n), "");
        set_qc(ir, overflow);
        return LLVMBuildSelect(B, overflow, value, wrap, "");
    }
    args[0] = a;
    args[1] = b;
    r = ir_intrinsic(ir, names[sub * 2 + u], t, args, 2); /* PADDS/PADDUS/PSUBS/PSUBUS for 8/16-bit lanes on x86 */
    set_qc(ir, LLVMBuildICmp(B, LLVMIntNE, r, wrap, ""));
    return r;
}

/* SHADD, UHADD, SRHADD, URHADD: (a + b [+ 1]) >> 1 without overflow. Unsigned 8/16-bit rounding is PAVGB/PAVGW on
   x86; signed goes through it too, biased by the lanes' sign bit (wunkolo, "pavgb: most-significant-bit constant"):
   (a ^ 0x80) and (b ^ 0x80) are a + 128 and b + 128, so their average is the signed one plus 128 */
static V halving(SnxIr* ir, unsigned u, unsigned rounding, unsigned size, unsigned n, V a, V b) {
    LLVMTypeRef t = LLVMTypeOf(a), wide = LLVMVectorType(lane_type(ir, size + 1), n);
    V bias = NULL, x, y, r;
    if (!u && size < 2) {
        bias = broadcast(ir, LLVMConstInt(lane_type(ir, size), (SnxU64)1 << ((8u << size) - 1), 0), n);
        a = LLVMBuildXor(B, a, bias, "");
        b = LLVMBuildXor(B, b, bias, "");
        u = 1;
    }
    x = u ? LLVMBuildZExt(B, a, wide, "") : LLVMBuildSExt(B, a, wide, "");
    y = u ? LLVMBuildZExt(B, b, wide, "") : LLVMBuildSExt(B, b, wide, "");
    r = LLVMBuildAdd(B, x, y, "");
    if (rounding) r = LLVMBuildAdd(B, r, broadcast(ir, LLVMConstInt(lane_type(ir, size + 1), 1, 0), n), "");
    r = u ? LLVMBuildLShr(B, r, broadcast(ir, LLVMConstInt(lane_type(ir, size + 1), 1, 0), n), "")
          : LLVMBuildAShr(B, r, broadcast(ir, LLVMConstInt(lane_type(ir, size + 1), 1, 0), n), "");
    r = LLVMBuildTrunc(B, r, t, "");
    return bias ? LLVMBuildXor(B, r, bias, "") : r;
}

/* <n x iN> all-ones or zero per lane, as the compares write them */
static V mask_of(SnxIr* ir, V condition, LLVMTypeRef lanes) { return LLVMBuildSExt(B, condition, lanes, ""); }

/* three-same integer (vector): simd_3same for BSL/BIT/BIF, compares, MIN/MAX, ABD/ABA, MLA/MLS, MUL, pairwise
   ADDP/MAXP/MINP */
static int three_same(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 11) & 31,
             rn = (w >> 5) & 31, rd = w & 31, eb = 8u << size, n, i;
    LLVMTypeRef t;
    V a, b, z, r;
    if (op == 3) { /* BSL, BIT, BIF (fast_simd has the others): one VPTERNLOGQ on AVX-512 hosts */
        LLVMTypeRef t = LLVMVectorType(ir->t_i64, 2);
        V x, y, d, r;
        if (!u || !size) return 0;
        x = ir_load(ir, t, vreg(ir, rn));
        y = ir_load(ir, t, vreg(ir, rm));
        d = ir_load(ir, t, vreg(ir, rd));
        LLVMSetAlignment(x, 8); /* V128 is 8-aligned in the CPU structure */
        LLVMSetAlignment(y, 8);
        LLVMSetAlignment(d, 8);
        if (host_ternlog(ir)) {
            unsigned table = size == 1   ? (TERN_C & TERN_A) | (~TERN_C & TERN_B)  /* BSL: (d & n) | (~d & m) */
                             : size == 2 ? (TERN_C & ~TERN_B) | (TERN_A & TERN_B)  /* BIT: (d & ~m) | (n & m) */
                                         : (TERN_C & TERN_B) | (TERN_A & ~TERN_B); /* BIF: (d & m) | (n & ~m) */
            r = LLVMBuildBitCast(B, ternlog(ir, x, y, d, table, 64), t, "");
        } else if (size == 1)
            r = LLVMBuildOr(B, LLVMBuildAnd(B, d, x, ""), LLVMBuildAnd(B, LLVMBuildNot(B, d, ""), y, ""), "");
        else if (size == 2)
            r = LLVMBuildOr(B, LLVMBuildAnd(B, d, LLVMBuildNot(B, y, ""), ""), LLVMBuildAnd(B, x, y, ""), "");
        else
            r = LLVMBuildOr(B, LLVMBuildAnd(B, d, y, ""), LLVMBuildAnd(B, x, LLVMBuildNot(B, y, ""), ""), "");
        if (!q) r = LLVMBuildInsertElement(B, r, ir_i64(ir, 0), ir_i32(ir, 1), "");
        LLVMSetAlignment(LLVMBuildStore(B, r, vreg(ir, rd)), 8);
        return 1;
    }
    if (op >= 0x18 || (size == 3 && !q)) return 0;
    n = (q ? 128u : 64u) / eb;
    t = LLVMVectorType(lane_type(ir, size), n);
    a = get_lanes(ir, vreg(ir, rn), size, n);
    b = get_lanes(ir, vreg(ir, rm), size, n);
    switch (op) {
        case 0x00:
        case 0x02:
            if (eb == 64) return 0;
            r = halving(ir, u, op == 0x02, size, n, a, b);
            break;
        case 0x01:
        case 0x05: r = saturating(ir, u, op == 0x05, eb, n, a, b); break;
        case 0x06: r = mask_of(ir, LLVMBuildICmp(B, u ? LLVMIntUGT : LLVMIntSGT, a, b, ""), t); break;
        case 0x07: r = mask_of(ir, LLVMBuildICmp(B, u ? LLVMIntUGE : LLVMIntSGE, a, b, ""), t); break;
        case 0x11:
            r = u ? mask_of(ir, LLVMBuildICmp(B, LLVMIntEQ, a, b, ""), t)
                  : mask_of(ir, LLVMBuildICmp(B, LLVMIntNE, LLVMBuildAnd(B, a, b, ""), LLVMConstNull(t), ""), t);
            break;
        case 0x0C:
        case 0x0D:
            if (eb == 64) return 0;
            r = LLVMBuildSelect(
                B,
                LLVMBuildICmp(B, op == 0x0C ? (u ? LLVMIntUGT : LLVMIntSGT) : (u ? LLVMIntULT : LLVMIntSLT), a, b, ""),
                a, b, "");
            break;
        case 0x0E:
        case 0x0F:
            if (eb == 64) return 0;
            r = LLVMBuildSelect(B, LLVMBuildICmp(B, u ? LLVMIntUGT : LLVMIntSGT, a, b, ""), LLVMBuildSub(B, a, b, ""),
                                LLVMBuildSub(B, b, a, ""), "");
            if (op == 0x0F) r = LLVMBuildAdd(B, get_lanes(ir, vreg(ir, rd), size, n), r, "");
            break;
        case 0x12:
            if (eb == 64) return 0;
            z = get_lanes(ir, vreg(ir, rd), size, n);
            r = u ? LLVMBuildSub(B, z, LLVMBuildMul(B, a, b, ""), "")
                  : LLVMBuildAdd(B, z, LLVMBuildMul(B, a, b, ""), "");
            break;
        case 0x13:
            if (eb == 64 || u) return 0; /* PMUL stays in snx_simd */
            r = LLVMBuildMul(B, a, b, "");
            break;
        case 0x14:
        case 0x15:
        case 0x17: {
            /* pairwise: lanes 0..n/2-1 from pairs of a, the rest from pairs of b */
            unsigned even[16], odd[16];
            V x, y;
            if ((op != 0x17 && eb == 64) || (op == 0x17 && u)) return 0;
            for (i = 0; i < n; ++i) {
                even[i] = i < n / 2 ? 2 * i : n + 2 * (i - n / 2);
                odd[i] = even[i] + 1;
            }
            x = shuffle(ir, a, b, even, n);
            y = shuffle(ir, a, b, odd, n);
            if (op == 0x17)
                r = LLVMBuildAdd(B, x, y, "");
            else
                r = LLVMBuildSelect(
                    B,
                    LLVMBuildICmp(B, op == 0x14 ? (u ? LLVMIntUGT : LLVMIntSGT) : (u ? LLVMIntULT : LLVMIntSLT), x, y,
                                  ""),
                    x, y, "");
            break;
        }
        default: return 0;
    }
    write_lanes(ir, rd, r, q);
    return 1;
}

/* byte-lane shifts by immediate through GF2P8AFFINEQB: simd_shimm's semantics for eb 8 */
static int shift_bytes_gfni(SnxIr* ir, unsigned q, unsigned u, unsigned op, unsigned shr, unsigned shl, unsigned rn,
                            unsigned rd) {
    unsigned n = q ? 16 : 8;
    V a = v128_type_load(ir, vreg(ir, rn)), r, z;
    switch (op) {
        case 0x00:
        case 0x02:
        case 0x04:
        case 0x06: /* [SU][R]SHR, [SU][R]SRA */
            r = gf_affine(ir, a, u ? gf_lshr(shr) : gf_ashr(shr));
            if (op & 4) /* + the last bit shifted out (signed or not, it is source bit shr-1) */
                r = LLVMBuildAdd(B, r, gf_affine(ir, a, gf_bit(shr - 1)), "");
            if (op & 2) r = LLVMBuildAdd(B, v128_type_load(ir, vreg(ir, rd)), r, "");
            break;
        case 0x08: /* SRI */
            if (!u) return 0;
            z = v128_type_load(ir, vreg(ir, rd));
            if (shr >= 8)
                r = z;
            else
                r = LLVMBuildOr(B, LLVMBuildAnd(B, z, broadcast(ir, ir_i8(ir, ~(0xFFu >> shr)), 16), ""),
                                gf_affine(ir, a, gf_lshr(shr)), "");
            break;
        case 0x0A:
            r = gf_affine(ir, a, gf_shl(shl));
            if (u) { /* SLI */
                z = v128_type_load(ir, vreg(ir, rd));
                r = LLVMBuildOr(B, LLVMBuildAnd(B, z, broadcast(ir, ir_i8(ir, ~(0xFFu << shl)), 16), ""), r, "");
            }
            break;
        default: return 0;
    }
    write_lanes(ir, rd, low_lanes(ir, r, n), q);
    return 1;
}

/* shift by immediate (vector): simd_shimm's SSHR/USHR, SSRA/USRA, SRSHR/URSHR, SRSRA/URSRA, SHL, SLI, SRI,
   SSHLL/USHLL, SHRN */
static int shift_immediate(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, immh = (w >> 19) & 15, immb = (w >> 16) & 7, op = (w >> 11) & 31,
             rn = (w >> 5) & 31, rd = w & 31, size = 3, eb, imm, shr, shl, n;
    LLVMTypeRef t;
    V a, r;
    if (!immh) return 0;
    while (!((immh >> size) & 1)) size--;
    eb = 8u << size;
    imm = (immh << 3) | immb;
    shr = 2 * eb - imm;
    shl = imm - eb;
    if (op == 0x14) { /* SSHLL, USHLL: the low or high half widened, shifted left */
        unsigned m;
        V x;
        if (size == 3) return 0;
        m = 64 / eb;
        x = get_lanes(ir, ir_cpu(ir, ir_off_v(rn) + (q ? 8 : 0)), size, m);
        t = LLVMVectorType(lane_type(ir, size + 1), m);
        x = u ? LLVMBuildZExt(B, x, t, "") : LLVMBuildSExt(B, x, t, "");
        r = LLVMBuildShl(B, x, broadcast(ir, LLVMConstInt(lane_type(ir, size + 1), shl, 0), m), "");
        set_lanes(ir, vreg(ir, rd), r);
        return 1;
    }
    if (op == 0x10 && !u) { /* SHRN, SHRN2: the high half keeps Vd's low half */
        unsigned m;
        V x;
        if (size == 3) return 0;
        m = 64 / eb;
        t = LLVMVectorType(lane_type(ir, size), m);
        x = get_lanes(ir, vreg(ir, rn), size + 1, m);
        x = LLVMBuildLShr(B, x, broadcast(ir, LLVMConstInt(lane_type(ir, size + 1), shr, 0), m), "");
        x = LLVMBuildTrunc(B, x, t, "");
        set_lanes(ir, ir_cpu(ir, ir_off_v(rd) + (q ? 8 : 0)), x);
        if (!q) zero_high(ir, rd);
        return 1;
    }
    if (size == 3 && !q) return 0;
    if (!size && host_gfni(ir)) return shift_bytes_gfni(ir, q, u, op, shr, shl, rn, rd);
    n = (q ? 128u : 64u) / eb;
    t = LLVMVectorType(lane_type(ir, size), n);
    a = get_lanes(ir, vreg(ir, rn), size, n);
#define SPLAT(v) broadcast(ir, LLVMConstInt(lane_type(ir, size), (v), 0), n)
    switch (op) {
        case 0x00:
        case 0x02:
        case 0x04:
        case 0x06: /* [SU][R]SHR, [SU][R]SRA: a shift by the element size gives 0 (unsigned) or the sign */
            if (u)
                r = shr >= eb ? LLVMConstNull(t) : LLVMBuildLShr(B, a, SPLAT(shr), "");
            else
                r = LLVMBuildAShr(B, a, SPLAT(shr >= eb ? eb - 1 : shr), "");
            if (op & 4) { /* rounding: + bit shr-1 of the source (for shr = eb, 0 signed, the top bit unsigned) */
                V bit = LLVMBuildAnd(B, LLVMBuildLShr(B, a, SPLAT(shr - 1), ""), SPLAT(1), "");
                r = shr >= eb && !u ? LLVMConstNull(t) : LLVMBuildAdd(B, r, bit, "");
            }
            if (op & 2) r = LLVMBuildAdd(B, get_lanes(ir, vreg(ir, rd), size, n), r, "");
            break;
        case 0x08: /* SRI */
            if (!u) return 0;
            if (shr >= eb)
                r = get_lanes(ir, vreg(ir, rd), size, n);
            else {
                SnxU64 m = (eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1) >> shr;
                r = LLVMBuildOr(B, LLVMBuildAnd(B, get_lanes(ir, vreg(ir, rd), size, n), SPLAT(~m), ""),
                                LLVMBuildLShr(B, a, SPLAT(shr), ""), "");
            }
            break;
        case 0x0A:
            if (!u) /* SHL */
                r = LLVMBuildShl(B, a, SPLAT(shl), "");
            else { /* SLI */
                SnxU64 m = (eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1) << shl;
                r = LLVMBuildOr(B, LLVMBuildAnd(B, get_lanes(ir, vreg(ir, rd), size, n), SPLAT(~m), ""),
                                LLVMBuildAnd(B, LLVMBuildShl(B, a, SPLAT(shl), ""), SPLAT(m), ""), "");
            }
            break;
        default: return 0;
    }
#undef SPLAT
    write_lanes(ir, rd, r, q);
    return 1;
}

/* FABS, FNEG (vector): sign bits only */
static int fp_sign_misc(SnxIr* ir, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, sz = size & 1;
    SnxU64 sign = sz ? (SnxU64)1 << 63 : ((SnxU64)0x80000000UL << 32) | 0x80000000UL;
    V lo, hi;
    if (op != 0x0F || !(size >> 1) || (sz && !q)) return 0;
    lo = ir_vd(ir, rn, 0);
    hi = ir_vd(ir, rn, 1);
    if (u) {
        lo = LLVMBuildXor(B, lo, ir_i64(ir, sign), "");
        hi = LLVMBuildXor(B, hi, ir_i64(ir, sign), "");
    } else {
        lo = LLVMBuildAnd(B, lo, ir_i64(ir, ~sign), "");
        hi = LLVMBuildAnd(B, hi, ir_i64(ir, ~sign), "");
    }
    ir_set_vd(ir, rd, 0, lo);
    ir_set_vd(ir, rd, 1, q ? hi : ir_i64(ir, 0));
    return 1;
}

int snx_ir_more_simd(SnxIr* ir, SnxU32 w) {
    if ((w & 0xBF800000UL) == 0x0C000000UL || (w & 0xBF800000UL) == 0x0C800000UL) return ld1_multiple(ir, w);
    if ((w & 0xBF000000UL) == 0x0D000000UL) return ld1_single(ir, w);
    if ((w & 0x9F200400UL) == 0x0E200400UL) return three_same(ir, w);
    if ((w & 0x9F3E0C00UL) == 0x0E200800UL) return byte_misc(ir, w) || fp_sign_misc(ir, w);
    if ((w & 0xBFE0FC00UL) == 0x0EE0E000UL) return pmull_1q(ir, w);
    if ((w & 0x9FF80400UL) == 0x0F000400UL) return modified_immediate(ir, w);
    if ((w & 0x9F800400UL) == 0x0F000400UL) return shift_immediate(ir, w);
    if ((w & 0xBF208C00UL) == 0x0E000800UL) return permute(ir, w);
    if ((w & 0xBFE08400UL) == 0x2E000000UL) return extract(ir, w);
    if ((w & 0xFFFF0C00UL) == 0x4E280800UL) return aes(ir, w);
    return 0;
}
