/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SIMD: snx_a64_fast_simd's shortcuts, the encodings of snx_simd that ir_simd.c emits inline (with snx_simd's
   semantics), and snx_simd itself for the rest. Written a lane at a time, as the templates' C is: every source lane is
   read before the first result is written (Vd may be Vn), and GCC's vectorizer turns the lanes into the host's
   vector instructions. ir_simd.c's x86-only forms (AES-NI, PCLMULQDQ, GFNI, VPTERNLOG) have no counterpart here: their
   encodings take the generic lanes or snx_simd. */
#include "jit.h"
#include "cpu.h"
#include <string.h>

typedef gcc_jit_rvalue* V;
typedef gcc_jit_type* T;

#define ADD(a, b) jit_op(j, GCC_JIT_BINARY_OP_PLUS, a, b)
#define SUB(a, b) jit_op(j, GCC_JIT_BINARY_OP_MINUS, a, b)
#define MUL(a, b) jit_op(j, GCC_JIT_BINARY_OP_MULT, a, b)
#define AND(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_AND, a, b)
#define OR(a, b)  jit_op(j, GCC_JIT_BINARY_OP_BITWISE_OR, a, b)
#define XOR(a, b) jit_op(j, GCC_JIT_BINARY_OP_BITWISE_XOR, a, b)
#define SHL(a, b) jit_op(j, GCC_JIT_BINARY_OP_LSHIFT, a, b)
#define SHR(a, b) jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, a, b)
#define NOT(a)    jit_unary(j, GCC_JIT_UNARY_OP_BITWISE_NEGATE, a)
#define EQ(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_EQ, a, b)
#define NE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_NE, a, b)
#define LT(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_LT, a, b)
#define GT(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_GT, a, b)
#define GE(a, b)  jit_cmp(j, GCC_JIT_COMPARISON_GE, a, b)
#define K32(v)    jit_u32(j, v)
#define K64(v)    jit_u64(j, v)

/* lanes of 1<<size bytes */
static T lane_type(SnxJit* j, unsigned size) { return jit_uint(j, 8u << size); }
static V klane(SnxJit* j, unsigned size, SnxU64 v) { return jit_k(j, lane_type(j, size), v); }
static void read_lanes(SnxJit* j, unsigned n, unsigned size, unsigned count, V* out) {
    unsigned i;
    for (i = 0; i < count; ++i) out[i] = jit_ve(j, n, size, i);
}
static void write_lanes(SnxJit* j, unsigned n, unsigned size, unsigned count, V const* values) {
    unsigned i;
    for (i = 0; i < count; ++i) jit_set_ve(j, n, size, i, values[i]);
}
static void zero_high(SnxJit* j, unsigned n) { jit_set_vd(j, n, 1, K64(0)); }
/* the lanes of Vd, the upper half cleared when Q is 0 (simd_perm, EXT, ... write r from zero) */
static void write_q(SnxJit* j, unsigned rd, unsigned size, unsigned count, V const* values, unsigned q) {
    write_lanes(j, rd, size, count, values);
    if (!q) zero_high(j, rd);
}
/* (uN)((iN)x >> s), x a lane of eb bits */
static V asr(SnxJit* j, V x, unsigned eb, unsigned s) {
    T i = jit_sint(j, eb);
    return jit_cast(j, jit_op(j, GCC_JIT_BINARY_OP_RSHIFT, jit_cast(j, x, i), jit_k(j, i, s)), jit_uint(j, eb));
}
static V as_signed(SnxJit* j, V x, unsigned eb) { return jit_cast(j, x, jit_sint(j, eb)); }
/* all ones or zero, as the compares write a lane */
static V mask_of(SnxJit* j, V condition, unsigned size) {
    return jit_sel(j, condition, klane(j, size, ~(SnxU64)0), klane(j, size, 0));
}
static V negate(SnxJit* j, V x, unsigned sz, int on) {
    return on ? XOR(x, sz ? K64((SnxU64)1 << 63) : K32(0x80000000UL)) : x;
}

/* ---- snx_a64_fast_simd ---- */

/* FMLA/FMLS/FMUL by element: r starts as Vd for the vector FMLA/FMLS, from zero for FMUL and the scalar forms, so
   only the vector FMLA/FMLS with Q keeps anything of Vd beyond the written lanes */
static void by_element_store(SnxJit* j, unsigned rd, unsigned s, unsigned lanes, V const* values, unsigned scalar,
                             unsigned q) {
    write_lanes(j, rd, s, lanes, values);
    if (scalar) {
        if (s == 2) jit_st(j, jit_off_v(rd) + 4, j->t_u32, K32(0));
        zero_high(j, rd);
    } else if (!q)
        zero_high(j, rd);
}

int snx_jit_fast_simd(SnxJit* j, SnxU32 w) {
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
                a0 = jit_vd(j, rn, 0);
                a1 = q ? jit_vd(j, rn, 1) : K64(0);
                jit_set_vd(j, rd, 0, a0);
                jit_set_vd(j, rd, 1, a1);
                return 1;
            }
            if (u && size) return 0;
            a0 = jit_vd(j, rn, 0);
            a1 = jit_vd(j, rn, 1);
            b0 = jit_vd(j, rm, 0);
            b1 = jit_vd(j, rm, 1);
            if (!u && (size & 1)) { /* &~, |~ */
                b0 = NOT(b0);
                b1 = NOT(b1);
            }
            if (u) {
                a0 = XOR(a0, b0);
                a1 = XOR(a1, b1);
            } else if (size < 2) {
                a0 = AND(a0, b0);
                a1 = AND(a1, b1);
            } else {
                a0 = OR(a0, b0);
                a1 = OR(a1, b1);
            }
            jit_set_vd(j, rd, 0, a0);
            jit_set_vd(j, rd, 1, q ? a1 : K64(0));
            return 1;
        }
        if (op >= 0x18) {
            int operation = -1, acc = 0, negated = 0;
            V a[4], b[4], r[4], na[4];
            unsigned s, width;
            gcc_jit_block *slow, *join;
            SnxJitFpFast f;
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
            s = sz ? 3 : 2;
            width = sz ? 64 : 32;
            read_lanes(j, rn, s, lanes, a);
            read_lanes(j, rm, s, lanes, b);
            if (acc) read_lanes(j, rd, s, lanes, r);
            for (i = 0; i < lanes; ++i) na[i] = negated ? jit_val(j, negate(j, a[i], sz, 1)) : a[i];
            /* every lane at once when every lane is exact (jit_fp_fast) */
            if (acc)
                jit_fp_fast(j, SNX_JIT_FP_FMA, width, lanes, na, b, r, &f);
            else
                jit_fp_fast(j, (unsigned)operation, width, lanes, a, b, NULL, &f);
            join = jit_fp_branch(j, f.ok, &slow);
            write_q(j, rd, s, lanes, f.value, q);
            jit_fp_else(j, join, slow, f.inexact);
            for (i = 0; i < lanes; ++i) {
                V value = acc ? jit_fp_fma(j, r[i], na[i], b[i], width)
                              : jit_fp_binary(j, a[i], b[i], width, (unsigned)operation);
                jit_set_ve(j, rd, s, i, value);
            }
            if (!q) zero_high(j, rd);
            jit_fp_join(j, join);
            return 1;
        }
        if (op == 0x10 && (q || size < 3)) { /* ADD, SUB (vector) */
            V a[16], b[16], r[16];
            lanes = (q ? 16 : 8) >> size;
            read_lanes(j, rn, size, lanes, a);
            read_lanes(j, rm, size, lanes, b);
            for (i = 0; i < lanes; ++i) r[i] = u ? SUB(a[i], b[i]) : ADD(a[i], b[i]);
            write_q(j, rd, size, lanes, r, q);
            return 1;
        }
        return 0;
    }
    if ((w & 0x9F000400UL) == 0x0F000000UL || (w & 0xDF000400UL) == 0x5F000000UL) { /* FMLA/FMLS/FMUL by element */
        V a[4], na[4], acc[4], e[4];
        unsigned s, width;
        gcc_jit_block *slow, *join;
        SnxJitFpFast f;
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
        width = sz ? 64 : 32;
        read_lanes(j, rn, s, lanes, a);
        if (op != 9) read_lanes(j, rd, s, lanes, acc); /* scalar: d0 */
        e[0] = jit_ve(j, m, s, index);
        for (i = 0; i < lanes; ++i) {
            e[i] = e[0];
            na[i] = op == 5 ? jit_val(j, negate(j, a[i], sz, 1)) : a[i];
        }
        if (op == 9)
            jit_fp_fast(j, SNX_JIT_FP_MUL, width, lanes, a, e, NULL, &f);
        else
            jit_fp_fast(j, SNX_JIT_FP_FMA, width, lanes, na, e, acc, &f);
        join = jit_fp_branch(j, f.ok, &slow);
        by_element_store(j, rd, s, lanes, f.value, scalar, q);
        jit_fp_else(j, join, slow, f.inexact);
        for (i = 0; i < lanes; ++i)
            f.value[i] = op == 9 ? jit_fp_binary(j, a[i], e[i], width, 2) : jit_fp_fma(j, acc[i], na[i], e[i], width);
        by_element_store(j, rd, s, lanes, f.value, scalar, q);
        jit_fp_join(j, join);
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
            if (rd != 31) jit_set_x(j, rd, jit_ve(j, rn, lane, index));
            return 1;
        }
        if (!u && imm4 == 3 && q) {
            jit_set_ve(j, rd, lane, index, jit_rx(j, rn));
            return 1;
        }
        if (u && q) {
            jit_set_ve(j, rd, lane, index, jit_ve(j, rn, lane, imm4 >> sz));
            return 1;
        }
        if (!u && (imm4 == 0 || imm4 == 1) && !(sz == 3 && !q)) {
            V e = !imm4 ? jit_ve(j, rn, lane, index) : jit_val(j, jit_cast(j, jit_rx(j, rn), lane_type(j, lane))),
              r[16];
            for (i = 0; i < lanes; ++i) r[i] = e;
            write_q(j, rd, lane, lanes, r, q);
            return 1;
        }
        return 0;
    }
    if ((w & 0xFFFFFFE0UL) == 0x2F00E400UL || (w & 0xFFFFFFE0UL) == 0x6F00E400UL) { /* MOVI Vd, #0 */
        jit_set_vd(j, rd, 0, K64(0));
        jit_set_vd(j, rd, 1, K64(0));
        return 1;
    }
    return 0;
}

/* SIMD_LDST, SIMD_LDST1, SIMD_DP, SIMD_DP2: if(!snx_simd(c,M,w)) c->rt->undef(c,pc,w); */
int snx_jit_simd_plain(SnxJit* j, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index) {
    gcc_jit_type* p[3];
    V args[3], handled;
    gcc_jit_block *undefined, *done;
    (void)index;
    if (strcmp(pattern->name, "SIMD_LDST") && strcmp(pattern->name, "SIMD_LDST1") && strcmp(pattern->name, "SIMD_DP") &&
        strcmp(pattern->name, "SIMD_DP2"))
        return 0;
    p[0] = j->t_ptr;
    p[1] = j->t_ptr;
    p[2] = j->t_u32;
    args[0] = j->c;
    args[1] = j->M;
    args[2] = K32(word);
    handled = jit_call(j, jit_extern(j, "snx_simd", j->t_int, 3, p), 3, args);
    undefined = jit_block(j);
    done = jit_block(j);
    jit_branch(j, jit_expect(j, EQ(handled, jit_int(j, 0)), 0), undefined, done);
    jit_enter(j, undefined);
    jit_undef(j, pc, word);
    jit_goto(j, done);
    jit_enter(j, done);
    return 1;
}

/* ---- beyond snx_a64_fast_simd: encodings snx_simd handles, emitted inline with its semantics ---- */

/* LD1/ST1 {Vt..Vt+n-1}: one copy per register, as 8-byte guest accesses. Post-index adds the size, or Xm */
static int ld1_multiple(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, op = (w >> 12) & 15, regs, bytes = q ? 16 : 8, r, k;
    V address;
    regs = op == 7 ? 1 : op == 10 ? 2 : op == 6 ? 3 : op == 2 ? 4 : 0;
    if (!regs) return 0;
    address = jit_x(j, rn);
    for (r = 0; r < regs; ++r)
        for (k = 0; k < bytes / 8; ++k) {
            V a = ADD(address, K64((SnxU64)r * bytes + 8 * k));
            if (l)
                jit_set_vd(j, (rt + r) & 31, k, jit_rd(j, a, 64));
            else
                jit_wr(j, a, 64, jit_vd(j, (rt + r) & 31, k));
        }
    if (l && !q)
        for (r = 0; r < regs; ++r) zero_high(j, (rt + r) & 31);
    if (post) jit_set_x(j, rn, ADD(address, rm == 31 ? K64((SnxU64)regs * bytes) : jit_x(j, rm)));
    return 1;
}

/* LD1R and LD1/ST1 of one lane (one register) */
static int ld1_single(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, l = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
             rt = w & 31, r = (w >> 21) & 1, op = (w >> 13) & 7, s = (w >> 12) & 1, size = (w >> 10) & 3;
    unsigned sc = op >> 1, esz, idx, i, n, bytes;
    V address;
    if ((op & 1) || r) return 0; /* LD2-4 and friends */
    address = jit_x(j, rn);
    if (sc == 3) { /* LD1R: the element in every lane, the rest zero */
        V value, lanes[16];
        if (!l || s) return 0;
        n = (q ? 128u : 64u) >> (3 + size);
        value = jit_rd(j, address, 8u << size);
        for (i = 0; i < n; ++i) lanes[i] = value;
        write_q(j, rt, size, n, lanes, q);
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
            jit_set_ve(j, rt, esz, idx, jit_rd(j, address, 8u << esz));
        else
            jit_wr(j, address, 8u << esz, jit_ve(j, rt, esz, idx));
        bytes = 1u << esz;
    }
    if (post) jit_set_x(j, rn, ADD(address, rm == 31 ? K64(bytes) : jit_x(j, rm)));
    return 1;
}

/* UZP1/2, TRN1/2, ZIP1/2: simd_perm's element formulas */
static int permute(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 7, rn = (w >> 5) & 31,
             rd = w & 31, n, h, part, i;
    V ab[32], r[16];
    if ((size == 3 && !q) || !(op & 3)) return 0;
    n = (q ? 128u : 64u) >> (3 + size);
    h = n / 2;
    part = (op >> 2) & 1;
    read_lanes(j, rn, size, n, ab);
    read_lanes(j, rm, size, n, ab + n);
    for (i = 0; i < n; ++i) {
        unsigned k;
        if ((op & 3) == 1) /* UZP: k < n ? a[k] : b[k-n] */
            k = 2 * i + part;
        else if ((op & 3) == 2) /* TRN */
            k = (i & 1) ? n + i / 2 * 2 + part : i / 2 * 2 + part;
        else /* ZIP */
            k = (i & 1) ? n + part * h + i / 2 : part * h + i / 2;
        r[i] = ab[k];
    }
    write_q(j, rd, size, n, r, q);
    return 1;
}

/* EXT: bytes imm4.. of Vn:Vm */
static int extract(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, rm = (w >> 16) & 31, imm4 = (w >> 11) & 15, rn = (w >> 5) & 31, rd = w & 31,
             len = q ? 16 : 8, i;
    V a[16], b[16], r[16];
    if (!q && (imm4 & 8)) return 0;
    read_lanes(j, rn, 0, len, a);
    read_lanes(j, rm, 0, len, b);
    for (i = 0; i < len; ++i) {
        unsigned source = imm4 + i;
        r[i] = source < len ? a[source] : b[source - len];
    }
    write_q(j, rd, 0, len, r, q);
    return 1;
}

static SnxU64 bytes_of(unsigned b) { return 0x0101010101010101ULL * (b & 0xFF); }
static V swap_bits(SnxJit* j, V x, unsigned shift, SnxU64 mask) {
    V m = K64(mask), s = K64(shift);
    return OR(AND(SHR(x, s), m), SHL(AND(x, m), s));
}

/* CNT, NOT, RBIT (byte lanes), on the 64-bit halves: a population count and a bit reversal per byte without crossing
   bytes (SWAR), which GCC's vectorizer has nothing to add to */
static int byte_misc(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, k;
    V x[2];
    if (op != 5 || !(u ? size < 2 : size == 0)) return 0;
    for (k = 0; k <= q; ++k) x[k] = jit_vd(j, rn, k);
    for (k = 0; k <= q; ++k) {
        V v = x[k];
        if (!u) { /* popcount per byte */
            v = jit_val(j, SUB(v, AND(SHR(v, K64(1)), K64(bytes_of(0x55)))));
            v = jit_val(j, ADD(AND(v, K64(bytes_of(0x33))), AND(SHR(v, K64(2)), K64(bytes_of(0x33)))));
            v = AND(ADD(v, SHR(v, K64(4))), K64(bytes_of(0x0F)));
        } else if (!size)
            v = NOT(v);
        else { /* bit reversal within each byte */
            v = jit_val(j, swap_bits(j, v, 1, bytes_of(0x55)));
            v = jit_val(j, swap_bits(j, v, 2, bytes_of(0x33)));
            v = swap_bits(j, v, 4, bytes_of(0x0F));
        }
        jit_set_vd(j, rd, k, v);
    }
    if (!q) zero_high(j, rd);
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
static int modified_immediate(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, op = (w >> 29) & 1, cmode = (w >> 12) & 15, rd = w & 31, o2 = (w >> 11) & 1;
    unsigned imm8 = (((w >> 16) & 7) << 5) | ((w >> 5) & 31);
    SnxU64 imm;
    if (o2 || (cmode == 15 && op && !q)) return 0;
    imm = expand_imm(op, cmode, imm8);
    if (cmode < 12 && (cmode & 1)) { /* ORR, BIC */
        V k = K64(op ? ~imm : imm), lo = jit_vd(j, rd, 0), hi = jit_vd(j, rd, 1);
        jit_set_vd(j, rd, 0, op ? AND(lo, k) : OR(lo, k));
        jit_set_vd(j, rd, 1, q ? (op ? AND(hi, k) : OR(hi, k)) : K64(0));
        return 1;
    }
    if (op && cmode != 14 && cmode != 15) imm = ~imm; /* MVNI */
    jit_set_vd(j, rd, 0, K64(imm));
    jit_set_vd(j, rd, 1, K64(q ? imm : 0));
    return 1;
}

/* FPSR.QC (bit 27) |= any saturated lane */
static void set_qc(SnxJit* j, V any) {
    V fpsr = jit_ld(j, offsetof(CPU, fpsr), j->t_u32);
    jit_st(j, offsetof(CPU, fpsr), j->t_u32,
           OR(fpsr, jit_op(j, GCC_JIT_BINARY_OP_LSHIFT, jit_cast(j, any, j->t_u32), K32(27))));
}

/* SQADD, UQADD, SQSUB, UQSUB (vector): the exact result in twice the lane's width (128 bits for 64-bit lanes),
   clamped to the lane's range; QC when a lane saturates (sqadd_ and friends) */
static V saturating(SnxJit* j, unsigned u, unsigned sub, unsigned eb, V a, V b, V* saturated) {
    unsigned wb = eb == 64 ? 128 : 2 * eb;
    T wide = u ? jit_uint(j, wb) : jit_sint(j, wb), lane = jit_uint(j, eb);
    V x = jit_cast(j, u ? a : as_signed(j, a, eb), wide), y = jit_cast(j, u ? b : as_signed(j, b, eb), wide), r, lo, hi;
    if (u) {
        lo = jit_k(j, wide, 0);
        hi = jit_k(j, wide, eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1); /* jit_k zero-extends to 128 bits */
        if (sub) {                                                          /* a < b: 0 */
            *saturated = LT(x, y);
            return jit_val(j, jit_sel(j, *saturated, jit_k(j, lane, 0), jit_cast(j, SUB(x, y), lane)));
        }
        r = jit_val(j, ADD(x, y));
        *saturated = GT(r, hi);
        return jit_val(j, jit_cast(j, jit_sel(j, *saturated, hi, r), lane));
    }
    hi = jit_cast(j, jit_k(j, jit_sint(j, eb), ((SnxU64)1 << (eb - 1)) - 1), wide);
    lo = jit_cast(j, jit_k(j, jit_sint(j, eb), (SnxU64)1 << (eb - 1)), wide);
    r = jit_val(j, sub ? SUB(x, y) : ADD(x, y));
    *saturated = jit_val(j, jit_lor(j, GT(r, hi), LT(r, lo)));
    return jit_val(j, jit_cast(j, jit_sel(j, GT(r, hi), hi, jit_sel(j, LT(r, lo), lo, r)), lane));
}

/* SHADD, UHADD, SRHADD, URHADD: (a + b [+ 1]) >> 1 in twice the lane's width */
static V halving(SnxJit* j, unsigned u, unsigned rounding, unsigned eb, V a, V b) {
    T wide = u ? jit_uint(j, 2 * eb) : jit_sint(j, 2 * eb);
    V r = ADD(jit_cast(j, u ? a : as_signed(j, a, eb), wide), jit_cast(j, u ? b : as_signed(j, b, eb), wide));
    if (rounding) r = ADD(r, jit_k(j, wide, 1));
    return jit_cast(j, SHR(r, jit_k(j, wide, 1)), jit_uint(j, eb));
}

/* a > b (or >=, <), signed or unsigned lanes */
static V compare(SnxJit* j, enum gcc_jit_comparison op, unsigned u, unsigned eb, V a, V b) {
    return u ? jit_cmp(j, op, a, b) : jit_cmp(j, op, as_signed(j, a, eb), as_signed(j, b, eb));
}

/* three-same integer (vector): simd_3same for BSL/BIT/BIF, compares, MIN/MAX, ABD/ABA, MLA/MLS, MUL, pairwise
   ADDP/MAXP/MINP, and the saturating and halving adds */
static int three_same(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 11) & 31,
             rn = (w >> 5) & 31, rd = w & 31, eb = 8u << size, n, i;
    V a[16], b[16], z[16], r[16], sat = NULL;
    if (op == 3) { /* BSL, BIT, BIF (fast_simd has the others) */
        unsigned k;
        V x[2], y[2], d[2];
        if (!u || !size) return 0;
        for (k = 0; k < 2; ++k) {
            x[k] = jit_vd(j, rn, k);
            y[k] = jit_vd(j, rm, k);
            d[k] = jit_vd(j, rd, k);
        }
        for (k = 0; k < 2; ++k) {
            if (size == 1) /* BSL: (d & n) | (~d & m) */
                r[k] = OR(AND(d[k], x[k]), AND(NOT(d[k]), y[k]));
            else if (size == 2) /* BIT: (d & ~m) | (n & m) */
                r[k] = OR(AND(d[k], NOT(y[k])), AND(x[k], y[k]));
            else /* BIF: (d & m) | (n & ~m) */
                r[k] = OR(AND(d[k], y[k]), AND(x[k], NOT(y[k])));
        }
        jit_set_vd(j, rd, 0, r[0]);
        jit_set_vd(j, rd, 1, q ? r[1] : K64(0));
        return 1;
    }
    if (op >= 0x18 || (size == 3 && !q)) return 0;
    n = (q ? 128u : 64u) / eb;
    switch (op) { /* the encodings handled here, before reading anything */
        case 0x00:
        case 0x02:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
        case 0x12:
        case 0x14:
        case 0x15:
            if (eb == 64) return 0;
            break;
        case 0x13:
            if (eb == 64 || u) return 0; /* PMUL stays in snx_simd */
            break;
        case 0x17:
            if (u) return 0;
            break;
        case 0x01:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x11: break;
        default: return 0;
    }
    read_lanes(j, rn, size, n, a);
    read_lanes(j, rm, size, n, b);
    if (op == 0x0F || op == 0x12) read_lanes(j, rd, size, n, z);
    for (i = 0; i < n; ++i) {
        V x = a[i], y = b[i];
        switch (op) {
            case 0x00:
            case 0x02: r[i] = halving(j, u, op == 0x02, eb, x, y); break;
            case 0x01:
            case 0x05: {
                V lane_sat;
                r[i] = saturating(j, u, op == 0x05, eb, x, y, &lane_sat);
                sat = sat ? jit_lor(j, sat, lane_sat) : lane_sat;
                break;
            }
            case 0x06: r[i] = mask_of(j, compare(j, GCC_JIT_COMPARISON_GT, u, eb, x, y), size); break;
            case 0x07: r[i] = mask_of(j, compare(j, GCC_JIT_COMPARISON_GE, u, eb, x, y), size); break;
            case 0x11: r[i] = mask_of(j, u ? EQ(x, y) : NE(AND(x, y), klane(j, size, 0)), size); break;
            case 0x0C:
            case 0x0D:
                r[i] = jit_sel(j, compare(j, op == 0x0C ? GCC_JIT_COMPARISON_GT : GCC_JIT_COMPARISON_LT, u, eb, x, y),
                               x, y);
                break;
            case 0x0E:
            case 0x0F:
                r[i] = jit_sel(j, compare(j, GCC_JIT_COMPARISON_GT, u, eb, x, y), SUB(x, y), SUB(y, x));
                if (op == 0x0F) r[i] = ADD(z[i], r[i]);
                break;
            case 0x12: r[i] = u ? SUB(z[i], MUL(x, y)) : ADD(z[i], MUL(x, y)); break;
            case 0x13: r[i] = MUL(x, y); break;
            default: { /* pairwise: lanes 0..n/2-1 from pairs of a, the rest from pairs of b */
                V* src = i < n / 2 ? a : b;
                unsigned k = 2 * (i < n / 2 ? i : i - n / 2);
                x = src[k];
                y = src[k + 1];
                if (op == 0x17)
                    r[i] = ADD(x, y);
                else
                    r[i] = jit_sel(
                        j, compare(j, op == 0x14 ? GCC_JIT_COMPARISON_GT : GCC_JIT_COMPARISON_LT, u, eb, x, y), x, y);
                break;
            }
        }
        r[i] = jit_val(j, jit_cast(j, r[i], lane_type(j, size)));
    }
    if (sat) set_qc(j, sat);
    write_q(j, rd, size, n, r, q);
    return 1;
}

/* shift by immediate (vector): simd_shimm's SSHR/USHR, SSRA/USRA, SRSHR/URSHR, SRSRA/URSRA, SHL, SLI, SRI,
   SSHLL/USHLL, SHRN */
static int shift_immediate(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, immh = (w >> 19) & 15, immb = (w >> 16) & 7, op = (w >> 11) & 31,
             rn = (w >> 5) & 31, rd = w & 31, size = 3, eb, imm, shr, shl, n, i;
    V a[16], z[16], r[16];
    if (!immh) return 0;
    while (!((immh >> size) & 1)) size--;
    eb = 8u << size;
    imm = (immh << 3) | immb;
    shr = 2 * eb - imm;
    shl = imm - eb;
    if (op == 0x14) { /* SSHLL, USHLL: the low or high half widened, shifted left */
        unsigned m;
        if (size == 3) return 0;
        m = 64 / eb;
        for (i = 0; i < m; ++i) a[i] = jit_ve(j, rn, size, (q ? m : 0) + i);
        for (i = 0; i < m; ++i) {
            V x = u ? jit_cast(j, a[i], lane_type(j, size + 1))
                    : jit_cast(j, as_signed(j, a[i], eb), jit_sint(j, 2 * eb));
            r[i] = SHL(jit_cast(j, x, lane_type(j, size + 1)), klane(j, size + 1, shl));
        }
        write_lanes(j, rd, size + 1, m, r);
        return 1;
    }
    if (op == 0x10 && !u) { /* SHRN, SHRN2: the high half keeps Vd's low half */
        unsigned m;
        if (size == 3) return 0;
        m = 64 / eb;
        read_lanes(j, rn, size + 1, m, a);
        for (i = 0; i < m; ++i) jit_set_ve(j, rd, size, (q ? m : 0) + i, SHR(a[i], klane(j, size + 1, shr)));
        if (!q) zero_high(j, rd);
        return 1;
    }
    if (size == 3 && !q) return 0;
    if ((op == 0x08 && !u) || (op != 0x00 && op != 0x02 && op != 0x04 && op != 0x06 && op != 0x08 && op != 0x0A))
        return 0;
    n = (q ? 128u : 64u) / eb;
    read_lanes(j, rn, size, n, a);
    if (op == 0x02 || op == 0x06 || op == 0x08 || (op == 0x0A && u)) read_lanes(j, rd, size, n, z);
    for (i = 0; i < n; ++i) {
        V x = a[i], v;
        SnxU64 all = eb == 64 ? ~(SnxU64)0 : ((SnxU64)1 << eb) - 1, m;
        switch (op) {
            case 0x00:
            case 0x02:
            case 0x04:
            case 0x06: /* [SU][R]SHR, [SU][R]SRA: a shift by the element size gives 0 (unsigned) or the sign */
                if (u)
                    v = shr >= eb ? klane(j, size, 0) : SHR(x, klane(j, size, shr));
                else
                    v = asr(j, x, eb, shr >= eb ? eb - 1 : shr);
                if (op & 4) { /* rounding: + bit shr-1 of the source (for shr = eb, 0 signed, the top bit unsigned) */
                    V bit = AND(SHR(x, klane(j, size, shr - 1)), klane(j, size, 1));
                    v = shr >= eb && !u ? klane(j, size, 0) : ADD(v, bit);
                }
                if (op & 2) v = ADD(z[i], v);
                break;
            case 0x08: /* SRI */
                if (shr >= eb)
                    v = z[i];
                else {
                    m = all >> shr;
                    v = OR(AND(z[i], klane(j, size, ~m)), SHR(x, klane(j, size, shr)));
                }
                break;
            default:
                if (!u) /* SHL */
                    v = SHL(x, klane(j, size, shl));
                else { /* SLI */
                    m = (all << shl) & all;
                    v = OR(AND(z[i], klane(j, size, ~m)), AND(SHL(x, klane(j, size, shl)), klane(j, size, m)));
                }
                break;
        }
        r[i] = jit_val(j, jit_cast(j, v, lane_type(j, size)));
    }
    write_q(j, rd, size, n, r, q);
    return 1;
}

/* FABS, FNEG (vector): sign bits only */
static int fp_sign_misc(SnxJit* j, SnxU32 w) {
    unsigned q = (w >> 30) & 1, u = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
             rd = w & 31, sz = size & 1;
    SnxU64 sign = sz ? (SnxU64)1 << 63 : ((SnxU64)0x80000000UL << 32) | 0x80000000UL;
    V lo, hi;
    if (op != 0x0F || !(size >> 1) || (sz && !q)) return 0;
    lo = jit_vd(j, rn, 0);
    hi = jit_vd(j, rn, 1);
    if (u) {
        lo = XOR(lo, K64(sign));
        hi = XOR(hi, K64(sign));
    } else {
        lo = AND(lo, K64(~sign));
        hi = AND(hi, K64(~sign));
    }
    jit_set_vd(j, rd, 0, lo);
    jit_set_vd(j, rd, 1, q ? hi : K64(0));
    return 1;
}

int snx_jit_more_simd(SnxJit* j, SnxU32 w) {
    if ((w & 0xBF800000UL) == 0x0C000000UL || (w & 0xBF800000UL) == 0x0C800000UL) return ld1_multiple(j, w);
    if ((w & 0xBF000000UL) == 0x0D000000UL) return ld1_single(j, w);
    if ((w & 0x9F200400UL) == 0x0E200400UL) return three_same(j, w);
    if ((w & 0x9F3E0C00UL) == 0x0E200800UL) return byte_misc(j, w) || fp_sign_misc(j, w);
    if ((w & 0x9FF80400UL) == 0x0F000400UL) return modified_immediate(j, w);
    if ((w & 0x9F800400UL) == 0x0F000400UL) return shift_immediate(j, w);
    if ((w & 0xBF208C00UL) == 0x0E000800UL) return permute(j, w);
    if ((w & 0xBFE08400UL) == 0x2E000000UL) return extract(j, w);
    return 0;
}
