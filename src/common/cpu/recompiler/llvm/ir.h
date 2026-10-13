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

typedef struct SnxIr {
    LLVMContextRef ctx;
    LLVMModuleRef mod;
    LLVMBuilderRef b;
    LLVMTargetMachineRef tm;
    int windows;
    LLVMTypeRef t_void, t_i1, t_i8, t_i16, t_i32, t_i64, t_i128, t_ptr, t_guest;
    /* the function being emitted: its CPU* and the guest memory base (u8* const M=c->mem) */
    LLVMValueRef fn, c, M;
    LLVMValueRef helpers[SNX_IR_HELPER_COUNT];
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
/* if (condition) { then() } else { otherwise() }: blocks positioned by the caller */
LLVMBasicBlockRef ir_block(SnxIr* ir, const char* name);
/* a stack slot in the entry block, as a C local whose address is taken */
LLVMValueRef ir_local(SnxIr* ir, LLVMTypeRef type);
LLVMTypeRef ir_v128(SnxIr* ir); /* [16 x i8] with V128's alignment */

#endif
