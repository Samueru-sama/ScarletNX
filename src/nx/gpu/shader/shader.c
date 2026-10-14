/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#include <string.h>
#include <ctype.h>
/* GM20B SASS -> GLSL 4.50. Instruction by instruction, the driver compiles the result and we cache it. */
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "gpu/shader/translator.h"

void sb_put(Sb* s, const char* t) {
    size_t l = strlen(t);
    if (s->n + l + 1 > s->cap) {
        s->cap = (s->cap + l + 1) * 2;
        s->p = realloc(s->p, s->cap);
    }
    memcpy(s->p + s->n, t, l + 1);
    s->n += l;
}
/* formatted text at the end of s, however long (no fixed buffer to cut a long expression off) */
static void sb_vf(Sb* s, const char* f, va_list ap) {
    va_list aq;
    va_copy(aq, ap);
    int l = vsnprintf(NULL, 0, f, aq);
    va_end(aq);
    if (l < 0) return;
    if (s->n + (size_t)l + 1 > s->cap) {
        s->cap = (s->n + (size_t)l + 1) * 2;
        s->p = realloc(s->p, s->cap);
    }
    vsnprintf(s->p + s->n, (size_t)l + 1, f, ap);
    s->n += (size_t)l;
}
void sb_f(Sb* s, const char* f, ...) {
    va_list ap;
    va_start(ap, f);
    sb_vf(s, f, ap);
    va_end(ap);
}
/* scratch string ring for building expressions; each slot grows to what it has to hold */
static __thread char* ring[64];
static __thread size_t ringcap[64];
static __thread int ringi;
char* T(const char* f, ...) {
    int i = ringi++ & 63;
    va_list ap, aq;
    va_start(ap, f);
    va_copy(aq, ap);
    int l = vsnprintf(NULL, 0, f, aq);
    va_end(aq);
    size_t need = l < 0 ? 1 : (size_t)l + 1;
    if (need > ringcap[i]) {
        ringcap[i] = need < 384 ? 384 : need;
        ring[i] = realloc(ring[i], ringcap[i]);
    }
    vsnprintf(ring[i], ringcap[i], f, ap);
    va_end(ap);
    return ring[i];
}
int shader_vk_set(int stage) { return stage == ST_FS ? 1 : (stage == ST_VS || stage == ST_CS) ? 0 : 2; }
/* where a declaration binds: its stage's set, and its binding in it (shader.h) */
static const char* bind_at(int stage, int binding) { return T("set=%d, binding=%d", shader_vk_set(stage), binding); }

/* registers past R254 (a vector access starting near the end) read as RZ, the way RZ itself does */
char* R(Ctx* c, u32 r) {
    if (r >= 255) return "0u";
    c->used_reg[r] = 1;
    return T("r%u", r);
}
char* RF(Ctx* c, u32 r) {
    if (r >= 255) return "0.0";
    c->used_reg[r] = 1;
    return T("uintBitsToFloat(r%u)", r);
}
char* P(Ctx* c, u32 p) {
    if (p == 7) return "true";
    c->used_pred[p] = 1;
    return T("p%u", p);
}
char* PN(Ctx* c, u32 p, int neg) {
    char* s = P(c, p);
    if (neg) return T("(!%s)", s);
    return s;
}
void shader_track_cbuf0_read(ShaderInfo* si, u32 off) {
    if (off != 0x640 && off != 0x644) si->cbuf0_draw_params_only = 0;
    if ((off & 3) || off / 4 >= 13 * 32)
        si->cbuf0_untracked_read = 1;
    else
        si->cbuf0_read_words[off / 128] |= 1u << ((off / 4) & 31);
}
char* CB(Ctx* c, u32 idx, u32 off) {
    c->si->cbuf_mask |= 1u << idx;
    if (idx == 0) shader_track_cbuf0_read(c->si, off);
    if (idx == 2) {
        if ((off & 3) || off / 4 >= 96)
            c->si->cbuf2_untracked_read = 1;
        else
            c->si->cbuf2_read_words[off / 128] |= 1u << ((off / 4) & 31);
    }
    if (idx < 18 && off + 16 > c->si->cbuf_max[idx]) c->si->cbuf_max[idx] = (off | 15) + 1;
    return T("floatBitsToUint(cb%u.d[%u].%c)", idx, off >> 4, "xyzw"[(off >> 2) & 3]);
}
char* CBF(Ctx* c, u32 idx, u32 off) {
    c->si->cbuf_mask |= 1u << idx;
    if (idx == 0) shader_track_cbuf0_read(c->si, off);
    if (idx == 2) {
        if ((off & 3) || off / 4 >= 96)
            c->si->cbuf2_untracked_read = 1;
        else
            c->si->cbuf2_read_words[off / 128] |= 1u << ((off / 4) & 31);
    }
    if (idx < 18 && off + 16 > c->si->cbuf_max[idx]) c->si->cbuf_max[idx] = (off | 15) + 1;
    return T("cb%u.d[%u].%c", idx, off >> 4, "xyzw"[(off >> 2) & 3]);
}
void emitl(Ctx* c, const char* f, ...) {
    va_list ap;
    for (int i = 0; i < c->indent; i++) sb_put(&c->out, "  ");
    va_start(ap, f);
    sb_vf(&c->out, f, ap);
    va_end(ap);
    sb_put(&c->out, "\n");
}
void setr(Ctx* c, u32 r, const char* e) {
    if (r >= 255) return; /* RZ (and past it): discard */
    c->used_reg[r] = 1;
    emitl(c, "r%u = %s;", r, e);
}
void setrf(Ctx* c, u32 r, const char* e) { setr(c, r, T("floatBitsToUint(%s)", e)); }
void setp(Ctx* c, u32 p, const char* e) {
    if (p == 7) return;
    c->used_pred[p] = 1;
    emitl(c, "p%u = %s;", p, e);
}
/* Both results consume the old combining predicate, even when pa aliases it. */
void setp2(Ctx* c, u32 pa, u32 pb, u32 bo, const char* t, const char* pc) {
    if (pa != 7 && pb != 7) {
        emitl(c, "{ bool t_ = %s, pred_ = %s;", t, pc);
        c->indent++;
        setp(c, pa, bop(bo, "t_", "pred_"));
        setp(c, pb, bop(bo, "(!t_)", "pred_"));
        c->indent--;
        emitl(c, "}");
    } else if (pa != 7)
        setp(c, pa, bop(bo, t, pc));
    else if (pb != 7)
        setp(c, pb, bop(bo, T("(!%s)", t), pc));
}

/* operand B per encoding */
u32 cb_idx(u64 w) { return B(w, 34, 5); }
u32 cb_off(u64 w) { return B(w, 20, 14) * 4; }
/* can c0[off] be the {address, size} of a storage buffer? A tag (off / 16 + 1) for the register that takes its address,
   or 0. The buffer is only taken (gbuf_use) when a global load, store or atomic goes through that register, so a c0
   word that holds something else costs no binding. NVN gives each stage 16 of them, 16-byte aligned, at its own place
   in c0 (Ryujinx GlobalMemory.GetStorageBaseCbOffset, MIT): 0x110 vertex, 0x210 tessellation control, 0x310
   tessellation evaluation and compute, 0x410 geometry, 0x510 fragment (Tears of the Kingdom's fragment shaders use
   0x550); deko3d puts the fragment ones at 0x730. The graphics stages take c0[0x110..0x830] and compute
   c0[0x110..0x410], which also holds deko3d's compute range at 0x110, up to the 16 the renderer binds (SNX_VKB_SSBO) */
u16 gbuf_tag_off(Ctx* c, u32 idx, u32 off) {
    u32 lo = 0x110, hi = c->stage == ST_CS ? 0x410 : 0x830;
    if (idx != 0 || off < lo || off >= hi || (off & 15)) return 0;
    return (u16)(off / 16 + 1);
}
u16 gbuf_tag(Ctx* c, u64 w) { return gbuf_tag_off(c, cb_idx(w), cb_off(w)); }
int gbuf_use(Ctx* c, u16 tag) {
    if (!tag) return -1;
    u32 off = (u32)(tag - 1) * 16;
    ShaderInfo* si = c->si;
    for (int i = 0; i < si->ngbuf; i++)
        if (si->gbuf_cbuf_off[i] == off) return i;
    if (si->ngbuf >= 16) return -1;
    si->gbuf_cbuf_off[si->ngbuf] = off;
    return si->ngbuf++;
}
char* imm20f(u64 w) {
    u32 v = (B(w, 20, 19) << 12) | (B(w, 56, 1) << 31);
    return T("uintBitsToFloat(0x%Xu)", v);
}
static u32 imm20i_v(u64 w) {
    u32 v = B(w, 20, 19);
    if (B(w, 56, 1)) v |= 0xFFF80000u;
    return v;
}
char* imm20i(u64 w) { return T("0x%Xu", imm20i_v(w)); }
char* imm32(u64 w) { return T("0x%Xu", B(w, 20, 32)); }
char* imm32f(u64 w) { return T("uintBitsToFloat(0x%Xu)", B(w, 20, 32)); }
/* kind: 0 reg(20) 1 cbuf 2 imm20 3 imm32 4 reg39 */
char* opB(Ctx* c, u64 w, int kind) {
    switch (kind) {
        case 0: return R(c, B(w, 20, 8));
        case 1: return CB(c, cb_idx(w), cb_off(w));
        case 2: return imm20i(w);
        case 3: return imm32(w);
        default: return R(c, B(w, 39, 8));
    }
}
char* opBf(Ctx* c, u64 w, int kind) {
    switch (kind) {
        case 0: return RF(c, B(w, 20, 8));
        case 1: return CBF(c, cb_idx(w), cb_off(w));
        case 2: return imm20f(w);
        case 3: return imm32f(w);
        default: return RF(c, B(w, 39, 8));
    }
}

char* fneg_abs(const char* e, int neg, int abs) {
    const char* x = e;
    if (abs) x = T("abs(%s)", x);
    if (neg) x = T("(-%s)", x);
    return (char*)x;
}
char* fsat(const char* e, int sat) { return sat ? T("clamp(%s,0.0,1.0)", e) : (char*)e; }
const char* fcmp_expr(u32 op, const char* a, const char* b) {
    switch (op) {
        case 0: return "false";
        case 1: return T("(%s < %s)", a, b);
        case 2: return T("(%s == %s)", a, b);
        case 3: return T("(%s <= %s)", a, b);
        case 4: return T("(%s > %s)", a, b);
        case 5: return T("(%s != %s && !isnan(%s) && !isnan(%s))", a, b, a, b);
        case 6: return T("(%s >= %s)", a, b);
        case 7: return T("(!isnan(%s) && !isnan(%s))", a, b);
        case 8: return T("(isnan(%s) || isnan(%s))", a, b);
        case 9: return T("(%s < %s || isnan(%s) || isnan(%s))", a, b, a, b);
        case 10: return T("(%s == %s || isnan(%s) || isnan(%s))", a, b, a, b);
        case 11: return T("(%s <= %s || isnan(%s) || isnan(%s))", a, b, a, b);
        case 12: return T("(%s > %s || isnan(%s) || isnan(%s))", a, b, a, b);
        case 13: return T("(%s != %s)", a, b);
        case 14: return T("(%s >= %s || isnan(%s) || isnan(%s))", a, b, a, b);
        default: return "true";
    }
}
const char* icmp_expr(u32 op, const char* a, const char* b, int sgn) {
    const char* A = sgn ? T("int(%s)", a) : a;
    const char* Bb = sgn ? T("int(%s)", b) : b;
    switch (op) {
        case 0: return "false";
        case 1: return T("(%s < %s)", A, Bb);
        case 2: return T("(%s == %s)", A, Bb);
        case 3: return T("(%s <= %s)", A, Bb);
        case 4: return T("(%s > %s)", A, Bb);
        case 5: return T("(%s != %s)", A, Bb);
        case 6: return T("(%s >= %s)", A, Bb);
        default: return "true";
    }
}
const char* bop(u32 op, const char* a, const char* b) {
    if (!strcmp(b, "true")) {
        if (op == 0) return a;
        if (op == 1) return "true";
        if (op == 2) return T("(!%s)", a);
    } /* PT */
    else if (!strcmp(b, "(!true)")) {
        if (op == 0) return "false";
        if (op == 1 || op == 2) return a;
    } /* !PT */
    switch (op) {
        case 0: return T("(%s && %s)", a, b);
        case 1: return T("(%s || %s)", a, b);
        case 2: return T("(%s != %s)", a, b);
    }
    return a;
}

/* ============================================================ walk */
static int is_term(int id) {
    return id == O_BRA || id == O_BRX || id == O_SYNC || id == O_BRK || id == O_CONT || id == O_EXIT || id == O_RET ||
           id == O_JMP;
}
static void mark(Ctx* c, u32 i) {
    if (i < 65536) c->leader[i >> 3] |= 1 << (i & 7);
}
static int is_leader(Ctx* c, u32 i) { return i < 65536 && (c->leader[i >> 3] >> (i & 7)) & 1; }
static void mark_tgt(Ctx* c, u32 i) {
    mark(c, i);
    if (i < 65536) c->tgt[i >> 3] |= 1 << (i & 7);
}
static int is_tgt(Ctx* c, u32 i) { return i < 65536 && (c->tgt[i >> 3] >> (i & 7)) & 1; }

int (*g_shader_cb_read)(u32 idx, u32 off, u32* out);
static int brx_targets(const u64* w, u32 i, u32 n, u32* out, int max);
u64 shader_hash(const u8* code, u32 max_bytes, u32* out_len) {
    if (__atomic_load_n(&ops_ready, __ATOMIC_ACQUIRE) != 2) ops_init();
    const u64* w = (const u64*)code;
    u32 n = max_bytes / 8;
    u32 i = 11, last = 11;
    /* linear scan until an unconditional EXIT past every branch target */
    u32 maxt = 0;
    int unresolved_brx = 0, has_cal = 0;
    u32 last_code = 11;
    for (u32 j = n; j-- > 11;)
        if (IDX_OK(j) && w[j]) {
            last_code = j;
            break;
        }
    for (; i < n; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(w[i]);
        last = i;
        if (!op) continue;
        if (op->id == O_BRA || op->id == O_CAL || op->id == O_SSY || op->id == O_PBK || op->id == O_PCNT) {
            u32 t = bra_target(w[i], i);
            if (t > maxt) maxt = t;
            if (op->id == O_CAL) has_cal = 1;
        }
        if (op->id == O_BRX) { /* its jump table's targets count too */
            u32 tg[64];
            int nt = brx_targets(w, i, n, tg, 64);
            if (nt < 0) unresolved_brx = 1;
            for (int q = 0; q < nt; q++)
                if (tg[q] > maxt) maxt = tg[q];
        }
        if (!unresolved_brx && op->id == O_EXIT && flow_always(w[i]) && i >= maxt) break;
        /* Unresolved BRX may reach later code; an all-zero tail bounds that scan. */
        if (unresolved_brx && op->id == O_EXIT && flow_always(w[i]) && i >= maxt && i >= last_code) break;
        if (!unresolved_brx && has_cal && op->id == O_RET && flow_always(w[i]) && i >= maxt) break;
        if (!unresolved_brx && op->id == O_BRA && flow_always(w[i]) && i >= maxt && bra_target(w[i], i) <= i) break;
        if (i > 16384) break;
    }
    u32 len = (last + 1) * 8;
    if (len > max_bytes) len = max_bytes;
    if (out_len) *out_len = len;
    u64 h = 1469598103934665603ull;
    for (u32 k = 0; k < len; k++) {
        h ^= code[k];
        h *= 1099511628211ull;
    }
    return h;
}

/* VertexA + VertexB (NVN dual vertex programs, slot 0 + slot 1): one program that runs A then B like the hardware.
   A's bundles go first and its EXITs become branches to B's first bundle; branches are relative so both keep working.
   B's header is used (declarations come from what the code touches). 0 if it doesn't fit in max_bytes */
int shader_merge_dual(const u8* a, const u8* b, u8* out, u32 max_bytes) {
    if (__atomic_load_n(&ops_ready, __ATOMIC_ACQUIRE) != 2) ops_init();
    u32 la, lb;
    shader_hash(a, max_bytes, &la);
    shader_hash(b, max_bytes, &lb);
    u32 bstart = 10 + ((la / 8 - 10 + 3) / 4) * 4; /* first bundle after A's last one */
    if ((size_t)bstart * 8 + (lb - 80) > max_bytes) return 0;
    memset(out, 0, max_bytes);
    memcpy(out, b, 80);
    memcpy(out + 80, a + 80, (size_t)bstart * 8 - 80);
    memcpy(out + (size_t)bstart * 8, b + 80, lb - 80);
    u64* w = (u64*)out;
    for (u32 i = 11; i < bstart; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(w[i]);
        if (!op || op->id != O_EXIT) continue;
        i32 off = ((i32)(bstart + 1) - (i32)(i + 1)) * 8; /* BRA keeps the EXIT's predicate and flow test */
        w[i] = 0xE240000000000000ull | ((u64)((u32)off & 0xFFFFFF) << 20) | (w[i] & (0xFull << 16)) | (w[i] & 31);
    }
    return 1;
}

/* A compiler-generated BRX table clamps its index, scales it by four, then loads an entry. */
static int brx_table_count(const u64* w, u32 ldc, u32* count) {
    if (B(w[ldc], 48, 3) != 4) return 0; /* LDC.B32 */
    u32 address = B(w[ldc], 8, 8);
    if (address == 255) return 0;
    for (u32 k = 1; k <= 32 && k < ldc; k++) {
        u32 j = ldc - k;
        if (!IDX_OK(j)) continue;
        const Op* op = decode(w[j]);
        if (!op || op->id == O_BRA || op->id == O_BRX || op->id == O_EXIT) break;
        if (B(w[j], 0, 8) != address) continue;
        if (op->id != O_SHL_I || B(w[j], 20, 5) != 2 || B(w[j], 16, 3) != 7 || B(w[j], 19, 1)) return 0;
        u32 index = B(w[j], 8, 8);
        for (u32 m = 1; m <= 32 && m < j; m++) {
            u32 p = j - m;
            if (!IDX_OK(p)) continue;
            const Op* prior = decode(w[p]);
            if (!prior || prior->id == O_BRA || prior->id == O_BRX || prior->id == O_EXIT) break;
            if (B(w[p], 0, 8) != index) continue;
            if (prior->id != O_IMNMX_I || B(w[p], 56, 1) || B(w[p], 16, 3) != 7 || B(w[p], 19, 1)) return 0;
            *count = B(w[p], 20, 19) + 1;
            return 1;
        }
        return 0;
    }
    return 0;
}

/* A distant LDC is usable only when every path from entry executes it and the BRX
   register stays intact. Keep this deliberately narrow: other instruction shapes
   fall back to the unresolved-BRX path. */
static int brx_far_ldc(const u64* w, u32 i, u32 ra) {
    u32 first = i > 64 ? i - 64 : 11;
    for (u32 j = i; j-- > first;) {
        if (!IDX_OK(j)) continue;
        const Op* op = decode(w[j]);
        if (!op) return -1;
        if (op->id == O_LDC && B(w[j], 0, 8) == ra) {
            if (B(w[j], 48, 3) != 4 || B(w[j], 16, 3) != 7 || B(w[j], 19, 1)) return -1;
            u32 count = 0;
            if (!brx_table_count(w, j, &count)) return -1;
            for (u32 p = 11; p < j; p++) {
                if (!IDX_OK(p)) continue;
                const Op* prior = decode(w[p]);
                if (!prior || is_term(prior->id) || prior->id == O_CAL || prior->id == O_SSY ||
                    prior->id == O_PBK || prior->id == O_PCNT || prior->id == O_PRET ||
                    prior->id == O_PEXIT || prior->id == O_BPT)
                    return -1;
            }
            return (int)j;
        }
        switch (op->id) {
            case O_NOP: case O_STG: case O_ISETP_I: break; /* no general-register result */
            case O_FMUL32I: case O_MUFU: case O_FMNMX_I: case O_MOV32I:
            case O_RRO_R: case O_FFMA_R: case O_FADD_R: case O_ICMP_CR:
            case O_FMUL_R: case O_MOV_R: case O_FADD_I:
                if (B(w[j], 0, 8) == ra) return -1;
                break;
            default: return -1;
        }
    }
    return -1;
}

/* BRX targets from its jump table (LDC nearby -> cbuf). -1 if the table cannot be bounded. */
static int brx_targets(const u64* w, u32 i, u32 n, u32* out, int max) {
    if (!g_shader_cb_read) return -1;
    u32 ra = B(w[i], 8, 8);
    i32 imm = (i32)(B(w[i], 20, 24) << 8) >> 8;
    u32 ldc = (u32)-1;
    for (u32 k = 1; k <= 8 && k < i; k++) {
        u32 j = i - k;
        if (!IDX_OK(j)) continue;
        const Op* o2 = decode(w[j]);
        if (!o2 || o2->id != O_LDC || B(w[j], 0, 8) != ra) continue;
        ldc = j;
        break;
    }
    if (ldc == (u32)-1) {
        int found = brx_far_ldc(w, i, ra);
        if (found < 0) return -1;
        ldc = (u32)found;
    }
    {
        u32 cidx = B(w[ldc], 36, 5);
        i32 coff = (i32)(B(w[ldc], 20, 16) << 16) >> 16;
        u32 table_count = 0;
        int exact = brx_table_count(w, ldc, &table_count);
        if (exact && table_count > (u32)max) return -1;
        int cnt = 0;
        for (u32 e = 0; e < (exact ? table_count : 64u) && cnt < max; e++) {
            u32 v;
            if (!g_shader_cb_read(cidx, (u32)(coff + (i32)e * 4), &v)) {
                if (exact) return -1;
                break;
            }
            i64 bb = (i64)(i32)v + imm;
            if (bb < 0 || (bb & 7)) {
                if (exact) return -1;
                break;
            }
            u64 t = (u64)i + 1 + (u64)(bb / 8);
            if (t >= 10 && ((t - 10) & 3) == 0) t++;
            if (t < 11) {
                if (exact) return -1;
                break;
            }
            if (t >= n) {
                if (exact) return -1;
                break;
            }
            out[cnt++] = (u32)t;
        }
        return cnt ? cnt : -1;
    }
    return -1;
}
/* ---- static SSY/PBK/PCNT resolution ----
   The hardware has one CRS stack: SSY/PBK/PCNT push a token of their kind with the target, SYNC/BRK/CONT pop the
   entries down to the nearest token of theirs (a BRK out of an SSY region drops the SSY entry). Walk the CFG carrying
   it. If every reachable block sees the same stack on all incoming paths, each SYNC/BRK/CONT has a fixed target and
   we can drop the stack. Any conflict (or no entry to pop, or depth > 16) falls back to the dynamic version. */
typedef struct {
    u8 n;
    u8 kind[16]; /* CRS_KIND */
    u32 s[16];
} Stk;
static int stk_eq(const Stk* a, const Stk* b) {
    return a->n == b->n && !memcmp(a->kind, b->kind, a->n) && !memcmp(a->s, b->s, a->n * 4);
}
typedef struct {
    Ctx* c;
    u32 n;
    int fwd;
    int* lid;
    Stk* st;
    u8* has;
    int* wl;
    int nwl;
} StkA;
/* edge i -> tg, 0 on conflict */
static int stk_succ(StkA* A, u32 i, u32 tg, const Stk* s) {
    if (A->fwd && tg <= i) return 1;                  /* linear mode: no later block -> return */
    if (tg >= A->n || !is_leader(A->c, tg)) return 1; /* no block -> return */
    int L = A->lid[tg];
    if (A->has[L]) return stk_eq(&A->st[L], s);
    A->st[L] = *s;
    A->has[L] = 1;
    A->wl[A->nwl++] = L;
    return 1;
}
static int stk_resolve(Ctx* c, u32 n, int fwd, const u32* brx_all, int brx_n) {
    int nl = 0;
    for (u32 i = 11; i < n; i++)
        if (IDX_OK(i) && is_leader(c, i)) nl++;
    StkA A = {c, n, fwd};
    A.lid = malloc(sizeof(int) * (n + 1));
    u32* ldr = malloc(sizeof(u32) * (nl + 1));
    A.st = malloc(sizeof(Stk) * (nl + 1));
    A.has = calloc(nl + 1, 1);
    A.wl = malloc(sizeof(int) * (nl + 1));
    int k = 0;
    for (u32 i = 11; i < n; i++)
        if (IDX_OK(i) && is_leader(c, i)) {
            A.lid[i] = k;
            ldr[k++] = i;
        }
    c->pop_tgt = calloc(n + 1, sizeof(u32));
    int ok = 1;
    Stk s0;
    memset(&s0, 0, sizeof s0);
    A.st[0] = s0;
    A.has[0] = 1;
    A.wl[A.nwl++] = 0;
    while (ok && A.nwl) {
        int L = A.wl[--A.nwl];
        Stk s = A.st[L];
        for (u32 i = ldr[L]; i < n && ok; i++) {
            if (!IDX_OK(i)) continue;
            if (i != ldr[L] && is_leader(c, i)) {
                ok = stk_succ(&A, i - 1, i, &s);
                break;
            } /* fall through */
            const Op* op = decode(c->w[i]);
            if (!op) continue;
            u64 w = c->w[i];
            int id = op->id;
            u32 pg = B(w, 16, 3), pn = B(w, 19, 1);
            int always = flow_always(w);
            int kk = CRS_KIND(id);
            if (id == O_SSY || id == O_PBK || id == O_PCNT) {
                if (s.n >= 16) {
                    ok = 0;
                    break;
                }
                s.kind[s.n] = (u8)kk;
                s.s[s.n++] = bra_target(w, i);
                continue;
            } /* unpredicated */
            if (pg == 7 && pn) continue; /* never runs */
            if (id == O_SYNC || id == O_BRK || id == O_CONT) {
                int j = s.n;
                while (j > 0 && s.kind[j - 1] != kk) j--;
                if (!j) {
                    ok = 0;
                    break;
                }
                u32 tg = s.s[j - 1];
                if (c->pop_tgt[i] && c->pop_tgt[i] != tg) {
                    ok = 0;
                    break;
                }
                c->pop_tgt[i] = tg;
                Stk s2 = s;
                s2.n = (u8)(j - 1);
                ok = stk_succ(&A, i, tg, &s2);
                if (always) break;
                continue;
            }
            if (id == O_BRA) {
                ok = stk_succ(&A, i, bra_target(w, i), &s);
                if (always) break;
                continue;
            }
            if (id == O_BRX) {
                for (int q = 0; q < brx_n && ok; q++) ok = stk_succ(&A, i, brx_all[q], &s);
                if (always) break;
                continue;
            }
            if (id == O_EXIT && always) break;
        }
    }
    free(A.lid);
    free(ldr);
    free(A.st);
    free(A.has);
    free(A.wl);
    return ok;
}

/* ---- structured emission for forward-only shaders (nested if/else, no pc var) ----
   forward branch i->t (BRA, or SYNC/BRK once resolved) opens 'if(!cond){' and closes at t. needs properly
   nested ranges; an unconditional branch right before a close turns into 'else'. otherwise use the pc guards. */
typedef struct {
    u32 end, end2;
    u8 els, in_else;
} Iv;
#define ST_MAX 64
/* target of branch i, 0 if not a branch, ~0u if it breaks structuring */
static u32 st_jump(Ctx* c, u32 i, const Op* op) {
    u64 w = c->w[i];
    int id = op->id;
    if (id != O_BRA && id != O_SYNC && id != O_BRK) return 0;
    if (B(w, 16, 3) == 7 && B(w, 19, 1)) return 0; /* never runs */
    /* A pop without a matching stack setup uses the dynamic CRS path. */
    if (id != O_BRA && !c->pop_tgt) return ~0u;
    u32 t = id == O_BRA ? bra_target(w, i) : c->pop_tgt[i];
    if (t <= i) return ~0u; /* backward branch or unreachable pop */
    u32 nx = i + 1;
    while (nx < c->nwords && !IDX_OK(nx)) nx++;
    return t == nx ? 0 : t;
} /* branch to i+1, no-op */
static int st_uncond(Ctx* c, u32 i) { return flow_always(c->w[i]); }
/* jt[i]: 0 normal, ~0u becomes 'else', otherwise the equivalent nearer target */
static int st_check(Ctx* c, u32 n, u32* jt) {
    Iv S[ST_MAX];
    int sp = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        while (sp && S[sp - 1].end == i) {
            if (S[sp - 1].els && !S[sp - 1].in_else) {
                S[sp - 1].in_else = 1;
                S[sp - 1].end = S[sp - 1].end2;
            } else
                sp--;
        }
        const Op* op = decode(c->w[i]);
        if (!op) continue;
        u32 t = st_jump(c, i, op);
        if (!t) continue;
        if (t == ~0u) return 0;
        if (sp && t > S[sp - 1].end) { /* jumps into the current if */
            Iv* tp = &S[sp - 1];
            u32 e = tp->end, g = e - 1;
            while (g > i && !IDX_OK(g)) g--;
            /* then-block ending in a jump to the same target: equivalent to targeting that jump */
            const Op* og = g > i && g < n ? decode(c->w[g]) : NULL;
            if (og && st_uncond(c, g) && st_jump(c, g, og) == t) {
                jt[i] = g;
                t = g;
            } else { /* uncond branch right before the close -> rest up to t is the else */
                u32 nx = i + 1;
                while (nx < n && !IDX_OK(nx)) nx++;
                if (st_uncond(c, i) && nx == e && e < n && !tp->els &&
                    (sp < 2 || (S[sp - 2].end > e && t <= S[sp - 2].end))) {
                    tp->els = 1;
                    tp->end2 = t;
                    jt[i] = ~0u;
                    continue;
                }
                return 0;
            }
        }
        if (sp >= ST_MAX) return 0;
        S[sp].end = t;
        S[sp].end2 = 0;
        S[sp].els = 0;
        S[sp].in_else = 0;
        sp++;
    }
    return 1;
}

char* shader_to_glsl(const u8* code, u32 max_bytes, int stage, ShaderInfo* si) {
    if (__atomic_load_n(&ops_ready, __ATOMIC_ACQUIRE) != 2) ops_init();
    Ctx* c = calloc(1, sizeof(Ctx));
    c->w = (const u64*)code;
    c->stage = stage;
    c->si = si;
    /* the key's inputs (the caller's masks, compute's group size and shared memory) survive the reset */
    u32 attr_int = si->attr_int_mask, attr_sint = si->attr_sint_mask, kclip = si->clip_enable;
    u32 ktm = si->tex_int_mask, kts = si->tex_sint_mask, krm = si->rt_int_mask, krs = si->rt_sint_mask;
    u32 kim = si->img_int_mask, kis = si->img_sint_mask;
    u32 kcl[3] = {si->cs_local[0], si->cs_local[1], si->cs_local[2]};
    int ksh = si->shared_size;
    memset(si, 0, sizeof *si);
    si->stage = stage;
    si->cbuf0_draw_params_only = 1;
    memcpy(si->cs_local, kcl, sizeof kcl);
    si->shared_size = ksh;
    si->attr_int_mask = attr_int;
    si->attr_sint_mask = attr_sint;
    si->clip_enable = kclip;
    si->tex_int_mask = ktm;
    si->tex_sint_mask = kts;
    si->rt_int_mask = krm;
    si->rt_sint_mask = krs;
    si->img_int_mask = kim;
    si->img_sint_mask = kis;
    memcpy(si->sph, code, 80);
    u32 len;
    shader_hash(code, max_bytes, &len);
    u32 n = len / 8;
    c->nwords = n;
    /* leaders + branch targets */
    mark(c, 11);
    int flow = 0, has_brx = 0, all_ldr = 0, has_stk = 0;
    for (u32 i = 11; i < n; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(c->w[i]);
        if (!op) continue;
        int id = op->id;
        if (id == O_BRA || id == O_SSY || id == O_PBK || id == O_PCNT) {
            mark_tgt(c, bra_target(c->w[i], i));
            flow = 1;
        }
        if (id == O_SSY || id == O_PBK || id == O_PCNT) has_stk = 1;
        if (id == O_BRX) {
            u32 tg[64];
            int nt = brx_targets(c->w, i, n, tg, 64);
            has_brx = 1; /* with a known table only its targets go in the switch */
            if (nt >= 0) {
                for (int q = 0; q < nt; q++) mark_tgt(c, tg[q]);
            } else {
                all_ldr = 1;
                for (u32 k = 11; k < n; k++)
                    if (IDX_OK(k)) mark_tgt(c, k);
            }
            flow = 1;
        }
        if (is_term(id) || id == O_BRA) {
            u32 nx = i + 1;
            while (nx < n && !IDX_OK(nx)) nx++;
            mark(c, nx);
            if (id != O_EXIT) flow = 1;
        }
    }
    c->has_flow = flow;
    /* all branches forward (no loops): skip the switch loop and emit blocks in order behind pc guards.
     compiles and runs a lot faster. a BRX with a forward-only table also works this way */
    int fwd = flow && !getenv("SNX_SHADER_SWITCH");
    for (u32 i = 11; i < n && fwd; i++) {
        if (!IDX_OK(i)) continue;
        const Op* op = decode(c->w[i]);
        if (!op) continue;
        int id = op->id;
        if (id == O_BRX) {
            u32 tg[64];
            int nt = brx_targets(c->w, i, n, tg, 64);
            if (nt < 0) fwd = 0;
            for (int q = 0; q < nt; q++)
                if (tg[q] <= i) fwd = 0;
        } else if (id == O_PCNT || id == O_CONT || id == O_CAL || id == O_RET || id == O_JMP)
            fwd = 0;
        else if ((id == O_BRA || id == O_SSY || id == O_PBK) && bra_target(c->w[i], i) <= i)
            fwd = 0;
    }
    /* resolve the stacks; a BRX can reach any block so all of them count as successors */
    if (flow && has_stk && !all_ldr && !getenv("SNX_SHADER_DYNSTACK")) {
        u32* bl = NULL;
        int nb = 0;
        if (has_brx) {
            bl = malloc(sizeof(u32) * (n + 1));
            for (u32 i = 11; i < n; i++)
                if (IDX_OK(i) && is_leader(c, i)) bl[nb++] = i;
        }
        c->stk_static = stk_resolve(c, n, fwd, bl, nb);
        free(bl);
    }
    /* linear, no BRX: only branch targets need a guard */
    int merge = fwd && !has_brx;
    u32* jt = NULL;
    int stru = 0;
    if (fwd && !has_brx && (!has_stk || c->stk_static) && !getenv("SNX_SHADER_NOSTRUCT")) {
        jt = calloc(n + 1, sizeof(u32));
        stru = st_check(c, n, jt);
    }
    /* body */
    c->indent = flow ? 3 : 1;
    size_t exit_end = (size_t)-1; /* where the last top level EXIT's text ends */
    if (stru) {
        Iv S[ST_MAX];
        int sp = 0;
        for (u32 i = 11; i < n; i++) {
            if (!IDX_OK(i)) continue;
            while (sp && S[sp - 1].end == i) {
                if (S[sp - 1].els && !S[sp - 1].in_else) {
                    c->indent = sp;
                    emitl(c, "} else {");
                    S[sp - 1].in_else = 1;
                    S[sp - 1].end = S[sp - 1].end2;
                } else {
                    sp--;
                    c->indent = sp + 1;
                    emitl(c, "}");
                }
            }
            c->indent = sp + 1;
            c->nest = sp; /* forward only, so writes in any branch already clear labels as we go */
            const Op* op = decode(c->w[i]);
            if (!op) {
                c->si->unsupported++;
                emitl(c, "/* unknown %016llx */", (unsigned long long)c->w[i]);
                continue;
            }
            u32 t = st_jump(c, i, op);
            if (t) {
                if (jt[i] != ~0u) {
                    u64 w = c->w[i];
                    u32 pg = B(w, 16, 3);
                    if (jt[i]) t = jt[i];
                    if (B(w, 0, 5) != 15) /* taken on a condition code too */
                        emitl(c, "if(!%s) {", flow_cond(c, w));
                    else
                        emitl(c, "if(%s) {", pg == 7 ? "false" : B(w, 19, 1) ? P(c, pg) : T("(!%s)", P(c, pg)));
                    S[sp].end = t;
                    S[sp].end2 = 0;
                    S[sp].els = 0;
                    S[sp].in_else = 0;
                    sp++;
                } else {
                    S[sp - 1].els = 1;
                    S[sp - 1].end2 = t;
                } /* branch ending the then -> else */
                continue;
            }
            if (op->id == O_BRA || op->id == O_SYNC || op->id == O_BRK) continue; /* never taken / goes to i+1 */
            emit_tracked(c, i, c->w[i], op);
            if (sp == 0 && op->id == O_EXIT && st_uncond(c, i)) exit_end = c->out.n;
        }
        while (sp) {
            sp--;
            c->indent = sp + 1;
            emitl(c, "}");
        }
        if (exit_end != c->out.n) {
            c->indent = 1;
            emitl(c, "return;");
        }
    } /* no second return after the final EXIT */
    else if (fwd)
        sb_put(&c->out, "  uint pc = 11u;\n");
    else if (flow) /* no iteration cap: a cut-off would return from a long loop past the outputs' writes */
        sb_put(&c->out, "  uint pc = 11u;\n  while(true){\n    switch(pc){\n");
    /* dead code still goes through translation (keeps ShaderInfo/decls the same), the text is thrown away */
    int dead = 0;
    Sb live = {0};
    for (u32 i = 11; i < n && !stru; i++) {
        if (!IDX_OK(i)) continue;
        int opens = flow && is_leader(c, i) && (i == 11 || !merge || is_tgt(c, i));
        if (opens && dead) {
            free(c->out.p);
            c->out = live;
            dead = 0;
        } else if (opens && fwd && i != 11) {
            c->indent = 3;
            emitl(c, "pc = %uu; break;", i);
        }
        if (opens && fwd) {
            if (i != 11) {
                c->indent = 2;
                emitl(c, "} while(false);");
            }
            c->indent = 2;
            emitl(c, "if(pc == %uu) do {", i);
            c->indent = 3;
        } else if (opens) {
            c->indent = 2;
            emitl(c, "case %uu:", i);
            c->indent = 3;
        } /* no 'pc = i; break;', fall through */
        if (is_leader(c, i)) memset(c->wtag, 0, sizeof c->wtag);
        const Op* op = decode(c->w[i]);
        if (!op) {
            c->si->unsupported++;
            emitl(c, "/* unknown %016llx */", (unsigned long long)c->w[i]);
            continue;
        }
        emit_tracked(c, i, c->w[i], op);
        int id = op->id;
        u64 w = c->w[i];
        if (!flow && id == O_EXIT && flow_always(w)) exit_end = c->out.n;
        if (flow && !dead && flow_always(w) &&
            (id == O_BRA || id == O_BRX || id == O_SYNC || id == O_BRK || id == O_CONT || id == O_EXIT)) {
            dead = 1;
            live = c->out;
            memset(&c->out, 0, sizeof c->out);
        }
    }
    if (dead) {
        free(c->out.p);
        c->out = live;
    }
    free(jt);
    if (stru) {
    } else if (fwd) {
        if (!dead) {
            c->indent = 3;
            emitl(c, "return;");
        }
        c->indent = 2;
        emitl(c, "} while(false);");
        c->indent = 1;
        emitl(c, "return;");
    } else if (flow) {
        if (!dead) {
            c->indent = 3;
            emitl(c, "return;");
        }
        /* a pc no block has (a BRX outside its table) ends the program like an EXIT */
        sb_put(&c->out, "      default:\n");
        c->indent = 4;
        exit_epilogue(c);
        sb_put(&c->out, "    }\n  }\n");
    } else if (exit_end != c->out.n) {
        c->indent = 1;
        exit_epilogue(c);
    } /* dead if we already ended with EXIT */
    /* declarations */
    Sb* D = &c->decl;
    sb_put(D, "#version 450\n");
    if (c->uses_draw_params) sb_put(D, "#extension GL_ARB_shader_draw_parameters : require\n");
    /* textureLod on array and cube shadow samplers (Tears of the Kingdom's shadow maps), which core GLSL leaves out; the
       SPIR-V it becomes is core Vulkan */
    if (c->uses_shadow_lod) sb_put(D, "#extension GL_EXT_texture_shadow_lod : require\n");
    /* gl_Layer from a stage before the fragment shader */
    if (stage != ST_FS && si->writes_layer)
        sb_put(D, "#extension GL_ARB_shader_viewport_layer_array : require\n#define SNX_SET_LAYER(v) gl_Layer=(v)\n");
    if (stage == ST_FS && si->uses_position_in)
        sb_f(D, "layout(std140, %s) uniform SNX_RS { vec4 snx_rs; };\n", bind_at(stage, SNX_VKB_RS)); /* 1/res scale */
    if (si->ngbuf) {
        if (stage == ST_CS)
            sb_put(D, "uint snx_sb_idx(uint b, uint word) { return word; }\n");
        else {
            /* The map starts with 16 list offsets, 16 binding word offsets and 16 word counts; page IDs follow. */
            sb_f(D, "layout(std430, %s) readonly buffer SNX_SMAP { uint d[]; } snx_smap;\n",
                 bind_at(stage, SNX_VKB_SMAP));
            sb_put(D, "uint snx_sb_idx(uint b, uint word) { uint h=snx_smap.d[b]; "
                      "if(h==0xffffffffu) return word; if(word>=snx_smap.d[32u+b]) return 0xffffffffu; "
                      "uint w=word+snx_smap.d[16u+b]; "
                      "return (snx_smap.d[h+(w>>10u)]<<10u)|(w&1023u); }\n");
        }
    }
    for (int i = 0; i < si->ngbuf; i++)
        sb_f(D, "layout(std430, %s) %sbuffer S%d_G%d { uint d[]; } g%d;\n", bind_at(stage, SNX_VKB_SSBO + i),
             ((si->gbuf_wmask >> i) & 1) ? "" : "readonly ", stage, i, i);
    if (stage == ST_CS)
        sb_f(D, "layout(local_size_x=%u, local_size_y=%u, local_size_z=%u) in;\n",
             si->cs_local[0] ? si->cs_local[0] : 1, si->cs_local[1] ? si->cs_local[1] : 1,
             si->cs_local[2] ? si->cs_local[2] : 1);
    for (int i = 0; i < 18; i++)
        if (si->cbuf_mask & (1u << i))
            sb_f(D, "layout(std140, %s) uniform S%d_CB%d { vec4 d[4096]; } cb%d;\n", bind_at(stage, SNX_VKB_CB + i),
                 stage, i, i);
    for (int i = 0; i < si->ntex; i++) {
        ShTex* t = &si->tex[i];
        sb_f(D, "layout(%s) uniform %s S%d_tex%d;\n#define tex%d S%d_tex%d\n", bind_at(stage, SNX_VKB_TEX + i),
             T("%s%s",
               (!t->shadow && ((si->tex_int_mask >> i) & 1)) ? (((si->tex_sint_mask >> i) & 1) ? "i" : "u") : "",
               samp_type(t->type, t->array, t->shadow)),
             stage, i, i, stage, i);
    } /* stage-unique name */
    if (stage == ST_VS) {
        for (int i = 0; i < 32; i++)
            if (c->in_used[i]) {
                si->in_mask |= 1u << i;
                int isint = (attr_int >> i) & 1;
                sb_f(D, "layout(location=%d) in %s in_attr%d;\n", i,
                     isint ? ((si->attr_sint_mask >> i) & 1 ? "ivec4" : "uvec4") : "vec4", i);
            }
        for (int i = 0; i < 32; i++)
            if (c->out_used[i]) sb_f(D, "layout(location=%d) out vec4 out_attr%d;\n", i, i);
        /* as many clip distances as the program writes: Vulkan clips with every one the array has, so an unwritten
           one would clip by an undefined value */
        int ncd = 0;
        while (ncd < 8 && si->uses_clip_mask >> ncd) ncd++;
        const char* cd = ncd ? T("float gl_ClipDistance[%d];", ncd) : "";
        /* invariant so depth matches across passes with different shaders */
        if (si->writes_layer) /* Mesa drops gl_Layer when the block is redeclared */
            sb_f(D, "invariant gl_Position;\n%s%s", ncd ? "out " : "", ncd ? T("%s\n", cd) : "");
        else
            sb_f(D, "out gl_PerVertex { invariant vec4 gl_Position; float gl_PointSize;%s%s };\n", ncd ? " " : "", cd);
    } else if (stage == ST_FS) {
        const u8* imap = (const u8*)si->sph + 0x18;
        for (int i = 0; i < 32; i++)
            if (c->in_used[i]) {
                si->in_mask |= 1u << i;
                /* the interpolation of the first component the header lists as used: 1 constant, 2 perspective,
                   3 screen linear (Ryujinx ImapPixelType.GetFirstUsedType) */
                u8 m = imap[i];
                int k = 0;
                while (k < 3 && !((m >> (k * 2)) & 3)) k++;
                int mode = (m >> (k * 2)) & 3, f = mode == 1, lin = mode == 3;
                sb_f(D, "layout(location=%d) %sin vec4 in_attr%d;\n", i, f ? "flat " : lin ? "noperspective " : "", i);
            }
        u32 target = si->sph[18];
        for (int rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15)
                sb_f(D, "layout(location=%d) out %s frag_color%d;\n", rt,
                     ((si->rt_int_mask >> rt) & 1) ? (((si->rt_sint_mask >> rt) & 1) ? "ivec4" : "uvec4") : "vec4", rt);
    }
    if (c->uses_local) sb_f(D, "uint lmem[%u];\n", lmem_words(c));
    if (c->uses_shared)
        sb_f(D, "shared uint smem[%d];\n", stage == ST_CS && si->shared_size > 0 ? (si->shared_size + 3) / 4 : 16384);
    if (stage == ST_CS || stage == ST_FS) {
        static const char* it[6] = {"image1D", "imageBuffer", "image1DArray", "image2D", "image2DArray", "image3D"};
        for (int k = 0; k < si->nimg; k++)
            sb_f(D, "layout(%s%s) %suniform %s%s img%d;\n",
                 si->img[k].number_type == 1 ? "r32ui, " : si->img[k].number_type == 2 ? "r32i, " : "",
                 bind_at(stage, SNX_VKB_IMG + k),
                 si->img[k].number_type ? "" : "writeonly ",
                 si->img[k].number_type == 1 ? "u" : si->img[k].number_type == 2 ? "i" : "",
                 it[si->img[k].type], k);
        if (c->uses_img_raw) {
            /* format class: 0 f32, 1 f16, 2 unorm8, 3 unorm16 */
            sb_put(
                D,
                T("layout(std140, %s) uniform SNX_FMT { int snx_ic[8]; int snx_tc[32]; int snx_ib[8]; int snx_tb[32]; };\n",
                  bind_at(stage, SNX_VKB_FMT)));
            sb_put(
                D,
                "vec4 snx_r2v(uvec4 r, uint n, uint sz, int cls){\n"
                "  if(sz<4u){ uint v = sz<2u? (r.x&0xFFu) : (r.x&0xFFFFu); if(cls==1) return vec4(unpackHalf2x16(v).x,0.0,0.0,1.0); if(cls==2) return vec4(float(v&0xFFu)/255.0,0.0,0.0,1.0); if(cls==3) return vec4(float(v)/65535.0,0.0,0.0,1.0); return vec4(uintBitsToFloat(v),0.0,0.0,1.0); }\n"
                "  if(cls==1) return n>=2u? vec4(unpackHalf2x16(r.x),unpackHalf2x16(r.y)) : vec4(unpackHalf2x16(r.x),0.0,1.0);\n"
                "  if(cls==2) return unpackUnorm4x8(r.x);\n"
                "  if(cls==3) return n>=2u? vec4(unpackUnorm2x16(r.x),unpackUnorm2x16(r.y)) : vec4(unpackUnorm2x16(r.x),0.0,1.0);\n"
                "  return vec4(uintBitsToFloat(r.x), n>=2u? uintBitsToFloat(r.y) : 0.0, n>=4u? uintBitsToFloat(r.z) : 0.0, n>=4u? uintBitsToFloat(r.w) : 1.0); }\n"
                "uvec4 snx_v2r(vec4 v, uint sz, int cls){\n"
                "  uvec4 o;\n"
                "  if(cls==1) o=uvec4(packHalf2x16(v.xy),packHalf2x16(v.zw),0u,0u); else if(cls==2) o=uvec4(packUnorm4x8(v),0u,0u,0u);\n"
                "  else if(cls==3) o=uvec4(packUnorm2x16(v.xy),packUnorm2x16(v.zw),0u,0u); else o=floatBitsToUint(v);\n"
                "  if(sz<2u) o.x&=0xFFu; else if(sz<4u) o.x&=0xFFFFu; return o; }\n");
        }
    }
    /* FMZ multiply: 0 * anything = 0, inf/NaN included */
    {
        const char* b = c->out.p ? c->out.p : "";
        int f1 = 0, f2 = strstr(b, "ffmz(") != NULL; /* only when needed */
        for (const char* q = b; (q = strstr(q, "fmz(")); q++)
            if (q == b || q[-1] != 'f') {
                f1 = 1;
                break;
            }
        if (f1) sb_put(D, "float fmz(float a, float b){ return (a==0.0 || b==0.0) ? 0.0 : a*b; }\n");
        if (f2) sb_put(D, "float ffmz(float a, float b, float c){ return (a==0.0 || b==0.0) ? c : fma(a,b,c); }\n");
    }
    sb_put(D, "void main(){\n");
    /* an output component the program doesn't write reads as that of (0, 0, 0, 1), as Ryujinx's translator (MIT)
       initializes them: Tears of the Kingdom's full-screen passes write the position's x, y and z only, and a w left
       undefined clipped them away */
    if (c->stage == ST_VS) {
        sb_put(D, "  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n");
        for (int i = 0; i < 32; i++)
            if (c->out_used[i]) sb_f(D, "  out_attr%d = vec4(0.0, 0.0, 0.0, 1.0);\n", i);
    }
    if (c->stage == ST_VS && !c->si->writes_point_size) sb_put(D, "  gl_PointSize = 1.0;\n");
    /* a clip distance below the last written one that the program leaves alone doesn't clip */
    for (int i = 0; c->stage == ST_VS && i < 8 && c->si->uses_clip_mask >> i; i++)
        if (!((c->si->uses_clip_mask >> i) & 1)) sb_f(D, "  gl_ClipDistance[%d] = 0.0;\n", i);
    /* the registers are precise in every stage. Maxwell runs the float math as the console's compiler scheduled it,
       neither fused nor reassociated, and a driver that does either changes answers: (a * c) * c folded into
       a * (c * c) skips a product that flushes to 0 (edenprobe fmath); Eden translates FADD and FMUL with
       no_contraction too. In a VS it also keeps a depth prepass and the color pass, different shaders with the
       same position math, from fusing an fma in only one of them, which breaks the EQUAL depth test.
       SNX_NO_PRECISE (or the older SNX_NO_PRECISE_VS) turns it off */
    {
        static int nop = -1;
        if (nop < 0) nop = getenv("SNX_NO_PRECISE") != NULL || getenv("SNX_NO_PRECISE_VS") != NULL;
        const char* rq = !nop ? "precise uint" : "uint";
        for (int i = 0; i < 255; i++)
            if (c->used_reg[i]) sb_f(D, "  %s r%d = 0u;\n", rq, i);
    }
    for (int i = 0; i < 7; i++)
        if (c->used_pred[i]) sb_f(D, "  bool p%d = false;\n", i);
    if (c->uses_cc) sb_put(D, "  bool cc_z=false, cc_s=false, cc_c=false, cc_o=false;\n");
    /* the CRS stack: target | token << 16 (CRS_KIND) */
    if (c->uses_crs) sb_put(D, "  uint crs[32]; int crs_sp=0;\n");
    if (stage == ST_FS) {
        u32 target = si->sph[18];
        for (int rt = 0; rt < 8; rt++)
            if ((target >> (rt * 4)) & 15)
                sb_f(D, "  frag_color%d = %s(0);\n", rt,
                     ((si->rt_int_mask >> rt) & 1) ? (((si->rt_sint_mask >> rt) & 1) ? "ivec4" : "uvec4") : "vec4");
    }
    sb_put(D, c->out.p ? c->out.p : "");
    sb_put(D, "}\n");
    char* r = D->p;
    free(c->out.p);
    free(c->pop_tgt);
    free(c);
    return r;
}
