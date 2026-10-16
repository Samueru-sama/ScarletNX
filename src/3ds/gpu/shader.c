/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/shader/shader_interpreter.cpp (RunInterpreter) and video_core/pica/shader_unit.cpp
   (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar
   a4b335e. Instruction fields from nihstro include/nihstro/shader_bytecode.h (BSD-3-Clause):

   Copyright 2014 Tony Wasserka
   All rights reserved.
   Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
   following conditions are met: Redistributions of source code must retain the above copyright notice, this list of
   conditions and the following disclaimer. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided
   with the distribution. Neither the name of the owner nor the names of its contributors may be used to endorse or
   promote products derived from this software without specific prior written permission.
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
   INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
   IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */
#include "3ds/gpu/shader.h"
#include <math.h>
#include <string.h>

#define PROGRAM_WORDS 4096u

/* opcodes (nihstro OpCode::Id) */
enum {
    OP_ADD = 0x00,
    OP_DP3 = 0x01,
    OP_DP4 = 0x02,
    OP_DPH = 0x03,
    OP_EX2 = 0x05,
    OP_LG2 = 0x06,
    OP_MUL = 0x08,
    OP_SGE = 0x09,
    OP_SLT = 0x0a,
    OP_FLR = 0x0b,
    OP_MAX = 0x0c,
    OP_MIN = 0x0d,
    OP_RCP = 0x0e,
    OP_RSQ = 0x0f,
    OP_MOVA = 0x12,
    OP_MOV = 0x13,
    OP_DPHI = 0x18,
    OP_SGEI = 0x1a,
    OP_SLTI = 0x1b,
    OP_BREAK = 0x20,
    OP_NOP = 0x21,
    OP_END = 0x22,
    OP_BREAKC = 0x23,
    OP_CALL = 0x24,
    OP_CALLC = 0x25,
    OP_CALLU = 0x26,
    OP_IFU = 0x27,
    OP_IFC = 0x28,
    OP_LOOP = 0x29,
    OP_EMIT = 0x2a,
    OP_SETEMIT = 0x2b,
    OP_JMPC = 0x2c,
    OP_JMPU = 0x2d,
    OP_CMP = 0x2e,
    OP_MADI = 0x30,
    OP_MAD = 0x38
};

/* the info table's instruction formats: arithmetic (format 1, with the inverted-source forms), multiply-add
   (format 5), or anything else (flow control, handled by opcode) */
typedef enum { KIND_OTHER, KIND_ARITH, KIND_ARITH_INVERTED, KIND_MAD } Kind;
static Kind kind_of(unsigned op) {
    if (op <= 0x0f || op == OP_MOVA || op == OP_MOV || op == OP_CMP || op == OP_CMP + 1) return KIND_ARITH;
    if (op >= 0x18 && op <= 0x1b) return KIND_ARITH_INVERTED;
    if (op >= 0x30) return KIND_MAD;
    return KIND_OTHER;
}
/* EffectiveOpCode: MAD and MADI ignore their low three bits, CMP its low bit */
static unsigned effective(unsigned op) {
    if ((op & ~7u) == OP_MAD) return OP_MAD;
    if ((op & ~7u) == OP_MADI) return OP_MADI;
    if ((op & ~1u) == OP_CMP) return OP_CMP;
    return op;
}

static unsigned bits(uint32_t v, unsigned at, unsigned n) { return (v >> at) & ((1u << n) - 1); }

/* boost::circular_buffer: pushing onto a full stack drops its oldest entry */
typedef struct {
    uint32_t else_address, end_address;
} IfEntry;
typedef struct {
    uint32_t end_address, return_address;
} CallEntry;
typedef struct {
    uint32_t entry_address, end_address;
    uint8_t downcounter, increment, previous_al;
} LoopEntry;
#define PUSH(stack, count, capacity, entry)                                                                            \
    do {                                                                                                               \
        if ((count) == (capacity)) {                                                                                   \
            memmove(&(stack)[0], &(stack)[1], sizeof((stack)[0]) * ((capacity) - 1));                                  \
            --(count);                                                                                                 \
        }                                                                                                              \
        (stack)[(count)++] = (entry);                                                                                  \
    } while (0)

static const float ones[4] = {1, 1, 1, 1};
static float discard[4];

/* SourceRegister: 0x00-0x0f inputs, 0x10-0x1f temporaries, 0x20-0x7f float uniforms. A uniform addressed through
   a0.x, a0.y or aL wraps at 128, an offset outside a signed byte counts as 0, and past the 96 uniforms reads ones */
static const float* source(const SnxCtrShaderSetup* s, const SnxCtrShaderUnit* u, unsigned reg, unsigned address) {
    if (reg < 0x10) return u->input[reg];
    if (reg < 0x20) return u->temporary[reg - 0x10];
    unsigned index = reg - 0x20;
    if (address) {
        int32_t offset = u->address[address - 1];
        if (offset < -128 || offset > 127) offset = 0;
        index = (unsigned)((int32_t)index + offset) & 0x7f;
        if (index >= 96) return ones;
    }
    return s->f[index];
}

/* the swizzle pattern's selectors for source n (1-3) and its negation bit */
static void swizzled(const float* v, uint32_t pattern, unsigned n, float out[4]) {
    static const unsigned select_at[3] = {5, 14, 23}, negate_at[3] = {4, 13, 22};
    unsigned base = select_at[n - 1];
    bool negate = (pattern >> negate_at[n - 1]) & 1;
    for (unsigned i = 0; i < 4; ++i) {
        /* component 0 is the selector's top two bits */
        float x = v[bits(pattern, base + (3 - i) * 2, 2)];
        out[i] = negate ? -x : x;
    }
}
static bool enabled(uint32_t pattern, unsigned i) { return (pattern & (0x8u >> i)) != 0; }

static float* destination(SnxCtrShaderUnit* u, unsigned reg) {
    if (reg < 0x10) return u->output[u->output_bank][reg];
    if (reg < 0x20) return u->temporary[reg - 0x10];
    return discard;
}

/* float to s32 as x86's truncating conversion gives it: out-of-range and NaN become INT32_MIN */
static int32_t to_s32(float f) {
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return INT32_MIN;
    return (int32_t)f;
}

static bool condition(const SnxCtrShaderUnit* u, uint32_t instr) {
    bool x = ((instr >> 25) & 1) == u->cc[0], y = ((instr >> 24) & 1) == u->cc[1];
    switch (bits(instr, 22, 2)) {
        case 0: return x || y;
        case 1: return x && y;
        case 2: return x;
        default: return y;
    }
}

static bool compare(unsigned op, float a, float b, bool old) {
    switch (op) {
        case 0: return a == b;
        case 1: return a != b;
        case 2: return a < b;
        case 3: return a <= b;
        case 4: return a > b;
        case 5: return a >= b;
        default: return old; /* unknown compare modes leave the code as it was */
    }
}

static void arithmetic(const SnxCtrShaderSetup* s, SnxCtrShaderUnit* u, uint32_t instr, unsigned op, bool inverted) {
    uint32_t pattern = s->swizzle[bits(instr, 0, 7)];
    unsigned address = bits(instr, 19, 2);
    unsigned reg1 = inverted ? bits(instr, 14, 5) : bits(instr, 12, 7);
    unsigned reg2 = inverted ? bits(instr, 7, 7) : bits(instr, 7, 5);
    float a[4], b[4], r;
    swizzled(source(s, u, reg1, inverted ? 0 : address), pattern, 1, a);
    swizzled(source(s, u, reg2, inverted ? address : 0), pattern, 2, b);
    float* d = destination(u, bits(instr, 21, 5));
    switch (op) {
        case OP_ADD:
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = a[i] + b[i];
            break;
        case OP_MUL:
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = snx_ctr_pica_mul(a[i], b[i]);
            break;
        case OP_FLR:
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = floorf(a[i]);
            break;
        case OP_MAX: /* this form keeps hardware's NaN results: max(0, NaN) is NaN, max(NaN, 0) is 0 */
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = a[i] > b[i] ? a[i] : b[i];
            break;
        case OP_MIN:
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = a[i] < b[i] ? a[i] : b[i];
            break;
        case OP_DP3:
        case OP_DP4:
        case OP_DPH:
        case OP_DPHI: {
            float dot = 0.0f;
            if (op == OP_DPH || op == OP_DPHI) a[3] = 1.0f;
            for (unsigned i = 0; i < (op == OP_DP3 ? 3u : 4u); ++i) dot = dot + snx_ctr_pica_mul(a[i], b[i]);
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = dot;
            break;
        }
        case OP_RCP:
        case OP_RSQ:
        case OP_EX2:
        case OP_LG2: /* the first component's result goes to every enabled component */
            r = op == OP_RCP   ? 1.0f / a[0]
                : op == OP_RSQ ? 1.0f / sqrtf(a[0])
                : op == OP_EX2 ? exp2f(a[0])
                               : log2f(a[0]);
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = r;
            break;
        case OP_MOVA:
            for (unsigned i = 0; i < 2; ++i)
                if (enabled(pattern, i)) u->address[i] = to_s32(a[i]);
            break;
        case OP_MOV:
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = a[i];
            break;
        case OP_SGE:
        case OP_SGEI:
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = a[i] >= b[i] ? 1.0f : 0.0f;
            break;
        case OP_SLT:
        case OP_SLTI:
            for (unsigned i = 0; i < 4; ++i)
                if (enabled(pattern, i)) d[i] = a[i] < b[i] ? 1.0f : 0.0f;
            break;
        case OP_CMP: /* x compares with bits 24..26, y with 21..23 */
            u->cc[0] = compare(bits(instr, 24, 3), a[0], b[0], u->cc[0]);
            u->cc[1] = compare(bits(instr, 21, 3), a[1], b[1], u->cc[1]);
            break;
        default: break; /* DST, LIT and the unassigned arithmetic opcodes are not implemented in Azahar either */
    }
}

static void multiply_add(const SnxCtrShaderSetup* s, SnxCtrShaderUnit* u, uint32_t instr, bool inverted) {
    uint32_t pattern = s->swizzle[bits(instr, 0, 5)];
    unsigned address = bits(instr, 22, 2);
    unsigned reg2 = inverted ? bits(instr, 12, 5) : bits(instr, 10, 7);
    unsigned reg3 = inverted ? bits(instr, 5, 7) : bits(instr, 5, 5);
    float a[4], b[4], c[4];
    swizzled(source(s, u, bits(instr, 17, 5), 0), pattern, 1, a);
    swizzled(source(s, u, reg2, inverted ? 0 : address), pattern, 2, b);
    swizzled(source(s, u, reg3, inverted ? address : 0), pattern, 3, c);
    float* d = destination(u, bits(instr, 24, 5));
    for (unsigned i = 0; i < 4; ++i)
        if (enabled(pattern, i)) d[i] = snx_ctr_pica_mul(a[i], b[i]) + c[i];
}

void snx_ctr_shader_run(const SnxCtrShaderSetup* s, SnxCtrShaderUnit* u, unsigned entry, SnxCtrEmitter* emitter) {
    IfEntry ifs[8];
    CallEntry calls[4];
    LoopEntry loops[4];
    unsigned if_count = 0, call_count = 0, loop_count = 0;
    uint32_t pc = entry;
    bool stop = false;
    while (!stop) {
        bool is_break = false;
        const uint32_t old_pc = pc;
        /* the last word of program memory always ends the program (Azahar's guard against unterminated geometry
           shaders) */
        uint32_t instr = pc < PROGRAM_WORDS - 1 ? s->code[pc] : (uint32_t)OP_END << 26;
        unsigned raw = instr >> 26, op = effective(raw);
        uint32_t dest = bits(instr, 10, 12), count = bits(instr, 0, 8);
        switch (kind_of(raw)) {
            case KIND_ARITH: arithmetic(s, u, instr, op, false); break;
            case KIND_ARITH_INVERTED: arithmetic(s, u, instr, op, true); break;
            case KIND_MAD: multiply_add(s, u, instr, op == OP_MADI); break;
            case KIND_OTHER:
                switch (raw) {
                    case OP_END: stop = true; break;
                    case OP_JMPC:
                        if (condition(u, instr)) pc = dest - 1;
                        break;
                    case OP_JMPU:
                        if (((s->b >> bits(instr, 22, 4)) & 1) == !(count & 1)) pc = dest - 1;
                        break;
                    case OP_CALL:
                    case OP_CALLU:
                    case OP_CALLC:
                        if (raw == OP_CALL || (raw == OP_CALLU && ((s->b >> bits(instr, 22, 4)) & 1)) ||
                            (raw == OP_CALLC && condition(u, instr))) {
                            CallEntry e = {dest + count, pc + 1};
                            PUSH(calls, call_count, 4u, e);
                            pc = dest - 1;
                        }
                        break;
                    case OP_IFU:
                    case OP_IFC: {
                        bool taken = raw == OP_IFU ? ((s->b >> bits(instr, 22, 4)) & 1) : condition(u, instr);
                        if (taken) {
                            IfEntry e = {dest, dest + count};
                            PUSH(ifs, if_count, 8u, e);
                        } else
                            pc = dest - 1;
                        break;
                    }
                    case OP_LOOP: {
                        const uint8_t* param = s->i[bits(instr, 22, 2)];
                        u->address[2] = param[1];
                        /* do_loop reads aL after it is set, so previous_aL is the new value, as in Azahar */
                        LoopEntry e = {pc + 1, dest + 1, param[0], param[2], (uint8_t)u->address[2]};
                        PUSH(loops, loop_count, 4u, e);
                        break;
                    }
                    case OP_BREAK: is_break = true; break;
                    case OP_BREAKC:
                        if (condition(u, instr)) is_break = true;
                        break;
                    case OP_EMIT:
                        if (emitter && emitter->emit) {
                            emitter->emit(emitter->context, emitter->vertex_id, emitter->prim_emit, emitter->winding,
                                          u->output[u->output_bank]);
                            u->output_bank = !u->output_bank;
                        }
                        break;
                    case OP_SETEMIT:
                        if (emitter) {
                            emitter->vertex_id = bits(instr, 24, 2);
                            emitter->prim_emit = bits(instr, 23, 1) != 0;
                            emitter->winding = bits(instr, 22, 1) != 0;
                        }
                        break;
                    default: break; /* NOP and the unassigned opcodes */
                }
                break;
        }
        ++pc;
        /* the scopes close in the order CALL, IF, LOOP. Several CALL scopes may end at once; a JMP at the end of a
           scope is never taken, hence the comparisons with old_pc + 1 */
        uint32_t next = old_pc + 1;
        for (unsigned i = 0; i < 4; ++i) {
            if (!call_count || calls[call_count - 1].end_address != next) break;
            /* hardware does not update the program counter when it closes the fourth scope at once */
            if (i < 3) {
                pc = calls[call_count - 1].return_address;
                next = pc;
            }
            --call_count;
        }
        if (if_count && ifs[if_count - 1].else_address == old_pc + 1) {
            pc = ifs[if_count - 1].end_address;
            --if_count;
        }
        if (loop_count && (loops[loop_count - 1].end_address == old_pc + 1 || is_break)) {
            LoopEntry* l = &loops[loop_count - 1];
            u->address[2] += l->increment;
            if (!is_break && l->downcounter--)
                pc = l->entry_address;
            else {
                pc = l->end_address;
                /* aL is restored only inside an enclosing loop */
                if (loop_count > 1) u->address[2] = l->previous_al;
                --loop_count;
            }
        }
    }
}

void snx_ctr_shader_load(const uint32_t* regs, unsigned block, SnxCtrShaderUnit* u, const float (*buffer)[4]) {
    unsigned max = regs[block + SNX_PICA_SH_INPUT_CONFIG] & 0xf;
    uint64_t map = ((uint64_t)regs[block + SNX_PICA_SH_ATTR_MAP_HIGH] << 32) | regs[block + SNX_PICA_SH_ATTR_MAP_LOW];
    for (unsigned attr = 0; attr <= max; ++attr) memcpy(u->input[(map >> (attr * 4)) & 15], buffer[attr], 16);
}

void snx_ctr_shader_store(const uint32_t* regs, unsigned block, const SnxCtrShaderUnit* u, float (*buffer)[4]) {
    uint32_t mask = regs[block + SNX_PICA_SH_OUTPUT_MASK] & 0xffff;
    unsigned n = 0;
    for (unsigned reg = 0; reg < 16; ++reg)
        if (mask & (1u << reg)) memcpy(buffer[n++], u->output[u->output_bank][reg], 16);
}
