/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The LLVM backend: A64 to LLVM IR through the LLVM-C API, mirroring what the portable backend prints.
   Each emitter builds the IR of the C the portable backend writes for the same instruction (a64_templates.inc,
   snx_a64_fast_simd, codegen.c), with the instruction word and pc constant as they are there. cpu.h's inline helpers
   become internal always-inline IR functions (ir.c); its out-of-line helpers are called in the runtime library
   (rtlib.c), which every module links. */
#ifndef SNX_RECOMPILER_LLVM_IR_H
#define SNX_RECOMPILER_LLVM_IR_H

#include "codegen.h"
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

/* cpu.h's inline functions and macro bodies, built on first use in each module */
enum {
    SNX_IR_ADDS64,
    SNX_IR_ADDS32,
    SNX_IR_SUBS64,
    SNX_IR_SUBS32,
    SNX_IR_LOGIC64,
    SNX_IR_LOGIC32,
    SNX_IR_COND_OK,
    SNX_IR_SET_NZCV,
    SNX_IR_ROR64,
    SNX_IR_ROR32,
    SNX_IR_SHF64,
    SNX_IR_SHF32,
    SNX_IR_EXT64,
    SNX_IR_CLZ64,
    SNX_IR_CLZ32,
    SNX_IR_CLS64,
    SNX_IR_CLS32,
    SNX_IR_RBIT64,
    SNX_IR_RBIT32,
    SNX_IR_REV16_64,
    SNX_IR_REV32_64,
    SNX_IR_BSWAP32,
    SNX_IR_BSWAP64,
    SNX_IR_UDIV64,
    SNX_IR_UDIV32,
    SNX_IR_SDIV64,
    SNX_IR_SDIV32,
    SNX_IR_UMULH64,
    SNX_IR_SMULH64,
    SNX_IR_BMASK,
    SNX_IR_UBFM,
    SNX_IR_SBFM,
    SNX_IR_BFM,
    SNX_IR_LDX,
    SNX_IR_STX,
    SNX_IR_LDXP,
    SNX_IR_STXP,
    SNX_IR_LOOKUP,
    SNX_IR_DC_ZVA,
    SNX_IR_VFP_IMM32,
    SNX_IR_VFP_IMM64,
    SNX_IR_HELPER_COUNT
};

/* Instruction set extensions the generated code may use. The host's are read once per process (snx_ir_init, from
   LLVM's CPUID-based host detection) into snx_ir_host; an SnxIr for another triple (tests) gets none of them. The
   x86 fields are only ever set when the target is x86-64, so an emitter gated on them is gated on the architecture */
typedef struct {
    int x86;                                  /* x86-64 target: x86 intrinsics are available at all */
    int sse42, crc32, pclmul, aes, gfni, fma; /* x86 extensions */
    int avx2, avx512bw, avx512vl, avx512bitalg;
} SnxIrFeatures;
extern SnxIrFeatures snx_ir_host;

typedef struct SnxIr {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef b;
    LLVMTargetMachineRef tm;
    int windows;
    const SnxIrFeatures* features;
    LLVMTypeRef t_void, t_i1, t_i8, t_i16, t_i32, t_i64, t_i128, t_ptr, t_guest;
    /* the function being emitted: its CPU* and the guest memory base (u8* const M=c->mem) */
    LLVMValueRef fn, c, M;
    LLVMValueRef helpers[SNX_IR_HELPER_COUNT];
    /* alias scopes: guest memory (through M) and the CPU structure never overlap */
    unsigned kind_scope, kind_noalias;
    LLVMValueRef scope_cpu, scope_guest;
    /* PLAIN instructions emitted as a call to the interpreter's snx_step, for patterns without an emitter */
    unsigned long fallbacks;
} SnxIr;

/* Once per process, before any other call; threads may then each own SnxIrs. 0, or -1 with the reason */
int snx_ir_init(char* error, size_t error_size);
/* opt is the portable backend's -O letter: 0,1,2,3,s,z */
SnxIr* snx_ir_new(const char* name, char opt, char* error, size_t error_size);
/* the same for another target triple (NULL: the host), for tests of the linkers; CPU and features stay generic */
SnxIr* snx_ir_new_target(const char* name, char opt, const char* triple, char* error, size_t error_size);
void snx_ir_free(SnxIr* ir);
/* Optimizes the module as Clang does at the -O level and returns the host object file (malloc'd) */
int snx_ir_object(SnxIr* ir, char opt, unsigned char** data, size_t* size, char* error, size_t error_size);
/* The module's IR as text (LLVMDisposeMessage) */
char* snx_ir_text(SnxIr* ir);

/* ---- functions (ir_codegen.c) ---- */
/* F_<address>, declared on first use, defined by snx_ir_emit */
LLVMValueRef snx_ir_guest_function(SnxIr* ir, SnxU64 address);
void snx_ir_emit(SnxIr* ir, SnxCodegen* codegen, const SnxFunction* function);
/* u64 name(CPU* c): one instruction at pc, as snx_a64_static_step writes it (tests and audit tools) */
LLVMValueRef snx_ir_static_step(SnxIr* ir, SnxU32 word, SnxU64 pc, const char* name);
/* Starts a function body: entry block, c and M */
void snx_ir_begin(SnxIr* ir, LLVMValueRef function);

/* ---- instructions (ir_a64.c, ir_simd.c, ir_fp.c) ---- */
/* The body of a PLAIN instruction (snx_a64_body with "c->rt->undef(c,pc,w)" for UNDEF, after snx_a64_fast_simd).
   0 when no emitter covers the pattern yet: the caller then calls snx_step */
int snx_ir_plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction);
/* The case's first operand of a flow instruction: the i1 condition of BC, the i64 target of BR/BLR/RET, the i32
   immediate of SVC/BRK */
LLVMValueRef snx_ir_flow_operand(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Case* instruction);
int snx_ir_simd_plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index);
int snx_ir_fp_plain(SnxIr* ir, SnxU32 word, SnxU64 pc, const SnxA64Pattern* pattern, size_t index);
/* snx_a64_fast_simd's shortcuts: 1 when it emitted the instruction */
int snx_ir_fast_simd(SnxIr* ir, SnxU32 word);
/* encodings of the SIMD patterns snx_simd handles that the backend also emits inline, with snx_simd's semantics
   (loads/stores, permutes, byte ops, immediates; AES and PMULL where CPUID shows the x86 extensions) */
int snx_ir_more_simd(SnxIr* ir, SnxU32 word);

/* ---- floating point fast paths (ir_fp.c) ----
   The host's IEEE arithmetic gives the soft-float helpers' exact result and flags when the rounding mode is
   to-nearest, the operands are normal and the result is well inside the normal range: no denormal, overflow,
   underflow or NaN can occur, FZ/DN/AHP don't apply, and the only flag is IXC. IXC is computed exactly (TwoSum for
   add/sub, an FMA residual for mul/div/sqrt where the host has FMA), or not needed: it is sticky, and once set an
   inexact result changes nothing. Everything else takes the helper call. */
enum { SNX_IR_FP_ADD, SNX_IR_FP_SUB, SNX_IR_FP_MUL, SNX_IR_FP_DIV, SNX_IR_FP_SQRT, SNX_IR_FP_FMA };
typedef struct {
    LLVMValueRef value;   /* result bits: iN, or <lanes x iN> */
    LLVMValueRef ok;      /* i1: the fast path is exact */
    LLVMValueRef inexact; /* i1: IXC to set */
} SnxFpFast;
/* a, b, add: bits as iN (lanes 1) or <lanes x iN>; FMA computes add + a*b, SQRT reads a */
void ir_fp_fast(SnxIr* ir, unsigned op, unsigned width, unsigned lanes, LLVMValueRef a, LLVMValueRef b,
                LLVMValueRef add, SnxFpFast* out);
/* if (ok) { fast } else { slow }: returns the join block, positions at the fast block; ir_fp_else moves to the slow
   block (setting IXC from inexact on the way out of the fast one), ir_fp_join to the join */
LLVMBasicBlockRef ir_fp_branch(SnxIr* ir, LLVMValueRef ok, LLVMBasicBlockRef* slow);
void ir_fp_else(SnxIr* ir, LLVMBasicBlockRef join, LLVMBasicBlockRef slow, LLVMValueRef inexact);
void ir_fp_join(SnxIr* ir, LLVMBasicBlockRef join);

/* ---- building blocks (ir.c) ---- */
LLVMValueRef ir_i8(SnxIr* ir, unsigned v);
LLVMValueRef ir_i16(SnxIr* ir, unsigned v);
LLVMValueRef ir_i32(SnxIr* ir, SnxU32 v);
LLVMValueRef ir_i64(SnxIr* ir, SnxU64 v);
/* integer conversions as C casts do them: to a narrower type truncates, to a wider one extends by sign or zero */
LLVMValueRef ir_cast(SnxIr* ir, LLVMValueRef v, LLVMTypeRef to, int is_signed);
LLVMValueRef ir_u32(SnxIr* ir, LLVMValueRef v);            /* (u32) */
LLVMValueRef ir_u64(SnxIr* ir, LLVMValueRef v);            /* (u64) from an unsigned value */
LLVMValueRef ir_s64(SnxIr* ir, LLVMValueRef v);            /* (u64)(i64) from a signed value */
LLVMValueRef ir_bool32(SnxIr* ir, LLVMValueRef condition); /* a C comparison's int */

/* pointer to the CPU field at byte offset */
LLVMValueRef ir_cpu(SnxIr* ir, size_t offset);
LLVMValueRef ir_load(SnxIr* ir, LLVMTypeRef type, LLVMValueRef pointer);
void ir_store(SnxIr* ir, LLVMValueRef value, LLVMValueRef pointer);
/* tags a load, store or atomic as an access to the CPU structure or to guest memory */
LLVMValueRef ir_tag_cpu(SnxIr* ir, LLVMValueRef access);
LLVMValueRef ir_tag_guest(SnxIr* ir, LLVMValueRef access);
LLVMValueRef ir_cpu_load(SnxIr* ir, LLVMTypeRef type, size_t offset);
void ir_cpu_store(SnxIr* ir, LLVMValueRef value, size_t offset);
size_t ir_off_x(unsigned n);
size_t ir_off_v(unsigned n);
size_t ir_off_pc(void);
size_t ir_off_cf(void);
size_t ir_off_excl_on(void);
size_t ir_off_fpcr(void);
size_t ir_off_mem(void);
LLVMValueRef ir_x(SnxIr* ir, unsigned n);                 /* c->x[n] */
void ir_set_x(SnxIr* ir, unsigned n, LLVMValueRef value); /* c->x[n]=(u64)value */
LLVMValueRef ir_rx(SnxIr* ir, unsigned n);                /* RX(n) */
void ir_setx(SnxIr* ir, unsigned n, LLVMValueRef value);  /* SETX(n,value) */
LLVMValueRef ir_vd(SnxIr* ir, unsigned n, unsigned lane); /* c->v[n].d[lane] */
void ir_set_vd(SnxIr* ir, unsigned n, unsigned lane, LLVMValueRef value);
/* c->v[n].<lane type>[index], element of 1<<size bytes */
LLVMValueRef ir_ve(SnxIr* ir, unsigned n, unsigned size, unsigned index);
void ir_set_ve(SnxIr* ir, unsigned n, unsigned size, unsigned index, LLVMValueRef value);
LLVMValueRef ir_pc(SnxIr* ir);
void ir_set_pc(SnxIr* ir, LLVMValueRef value);

/* guest memory: MP(a), RDn(a) as iN, WRn(a,v) storing (uN)v, RDQ/WRQ as 16-byte copies */
LLVMValueRef ir_mp(SnxIr* ir, LLVMValueRef address);
LLVMValueRef ir_rd(SnxIr* ir, LLVMValueRef address, unsigned bits);
void ir_wr(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value);
/* n bytes as <n x i8> */
LLVMValueRef ir_rd_bytes(SnxIr* ir, LLVMValueRef address, unsigned bytes);
void ir_wr_bytes(SnxIr* ir, LLVMValueRef address, LLVMValueRef value);
void ir_rdq(SnxIr* ir, LLVMValueRef destination, LLVMValueRef address); /* RDQ(*destination, address) */
void ir_wrq(SnxIr* ir, LLVMValueRef address, LLVMValueRef source);      /* WRQ(address, *source) */
LLVMValueRef ir_ldacq(SnxIr* ir, LLVMValueRef address, unsigned bits);  /* LDACQ(uN,a) */
void ir_strel(SnxIr* ir, LLVMValueRef address, unsigned bits, LLVMValueRef value);

LLVMValueRef ir_helper(SnxIr* ir, int id);
LLVMValueRef ir_call(SnxIr* ir, LLVMValueRef function, LLVMValueRef* args, unsigned count);
LLVMValueRef ir_call_helper(SnxIr* ir, int id, LLVMValueRef* args, unsigned count);
/* an out-of-line function of the runtime library (cpu_helpers.c, rtlib.c), declared on first use */
LLVMValueRef ir_extern(SnxIr* ir, const char* name, LLVMTypeRef ret, LLVMTypeRef* params, unsigned count);
/* c->rt-><field>(args...) */
LLVMValueRef ir_rt_call(SnxIr* ir, size_t offset, LLVMTypeRef ret, LLVMTypeRef* params, LLVMValueRef* args,
                        unsigned count);
size_t ir_rt_svc(void);
size_t ir_rt_undef(void);
size_t ir_rt_note_hint(void);
size_t ir_rt_brk(void);
size_t ir_rt_sev(void);
size_t ir_rt_wfe(void);
void ir_undef(SnxIr* ir, SnxU64 pc, SnxU32 word); /* c->rt->undef(c,pc,w) */
void ir_fence(SnxIr* ir);                         /* SNX_FENCE() */
void ir_yield(SnxIr* ir);                         /* YIELD() */
/* branch weights: the condition is expected false (UNLIKELY) */
void ir_unlikely(SnxIr* ir, LLVMValueRef branch);
/* a call to an LLVM intrinsic, overloaded on type (NULL: not overloaded) */
LLVMValueRef ir_intrinsic(SnxIr* ir, const char* name, LLVMTypeRef type, LLVMValueRef* args, unsigned count);
/* memory effects of a declared function: none (pure), or only memory reached through its pointer arguments */
void ir_pure(SnxIr* ir, LLVMValueRef function);
void ir_argmem(SnxIr* ir, LLVMValueRef function);
/* if (condition) { then() } else { otherwise() }: blocks positioned by the caller */
LLVMBasicBlockRef ir_block(SnxIr* ir, const char* name);
/* a stack slot in the entry block, as a C local whose address is taken */
LLVMValueRef ir_local(SnxIr* ir, LLVMTypeRef type);
LLVMTypeRef ir_v128(SnxIr* ir); /* [16 x i8] with V128's alignment */

#endif
