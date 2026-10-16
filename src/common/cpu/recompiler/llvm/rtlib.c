/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of every game module the backends don't generate per game: the helpers (cpu_helpers.c), the fallback
   interpreter, and the C library functions LLVM lowers memory
   intrinsics to. It is compiled once, when the recompiler is built, with the project's Clang and the flags
   toolchain.c gives generated C (-march=native: build == host == target), and linked into each
   module by link_elf.c / link_coff.c. The modules import nothing, so they need no dynamic symbol resolution.
   It has no writable globals (a miss goes to RtAPI.miss), so the runtime JIT loads a copy of its own into the code
   arena (jit_service.c) without anything being kept twice */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/aarch64/cpu_helpers.c"

/* snx-rtlib-gen writes the interpreter module.c holds (snx_a64_interp) with external linkage, so the module IR can
   point MOD.interp at it. The step function (snx_a64_interp step mode) is no longer here: every pattern has an IR
   emitter, and the tests that compare against it generate their own */
#include "rtlib_interp.inc"

/* LLVM turns large or variable copies into these calls, and Clang does inside cpu_helpers.c. no_builtin keeps the
   loops from being turned back into calls to themselves */
__attribute__((no_builtin)) void* memcpy(void* RESTRICT d, const void* RESTRICT s, size_t n) {
    u8* p = (u8*)d;
    const u8* q = (const u8*)s;
    while (n--) *p++ = *q++;
    return d;
}
__attribute__((no_builtin)) void* memmove(void* d, const void* s, size_t n) {
    u8* p = (u8*)d;
    const u8* q = (const u8*)s;
    if (p < q)
        while (n--) *p++ = *q++;
    else
        while (n--) p[n] = q[n];
    return d;
}
__attribute__((no_builtin)) void* memset(void* d, int v, size_t n) {
    u8* p = (u8*)d;
    while (n--) *p++ = (u8)v;
    return d;
}
__attribute__((no_builtin)) int memcmp(const void* a, const void* b, size_t n) {
    const u8 *p = (const u8*)a, *q = (const u8*)b;
    for (; n; --n, ++p, ++q)
        if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}

/* 128-bit division, which Clang leaves to libgcc or compiler-rt (fp_arithmetic.c divides a 128-bit numerator by a
   64-bit divisor). The module doesn't link either library, so it carries its own */
typedef unsigned __int128 u128;
static u64 div_wide(u64 hi, u64 lo, u64 d, u64* r) { /* hi < d */
#if defined(__x86_64__)
    u64 q;
    __asm__("divq %4" : "=a"(q), "=d"(*r) : "a"(lo), "d"(hi), "rm"(d));
    return q;
#else
    u128 n = ((u128)hi << 64) | lo, rem = 0;
    u64 q = 0;
    int i;
    for (i = 127; i >= 0; --i) {
        rem = (rem << 1) | ((n >> i) & 1);
        if (rem >= d) {
            rem -= d;
            if (i < 64) q |= (u64)1 << i;
        }
    }
    *r = (u64)rem;
    return q;
#endif
}
static u128 divmod128(u128 n, u128 d, u128* rem) {
    u64 dh = (u64)(d >> 64), dl = (u64)d, nh = (u64)(n >> 64), r;
    u128 q = 0, rest = 0;
    int i;
    if (!dh) {
        u64 qh = nh / dl, ql = div_wide(nh % dl, (u64)n, dl, &r);
        *rem = r;
        return ((u128)qh << 64) | ql;
    }
    for (i = 127; i >= 0; --i) {
        rest = (rest << 1) | ((n >> i) & 1);
        if (rest >= d) {
            rest -= d;
            q |= (u128)1 << i;
        }
    }
    *rem = rest;
    return q;
}
u128 __udivti3(u128 n, u128 d);
u128 __umodti3(u128 n, u128 d);
u128 __udivti3(u128 n, u128 d) {
    u128 r;
    return divmod128(n, d, &r);
}
u128 __umodti3(u128 n, u128 d) {
    u128 r;
    divmod128(n, d, &r);
    return r;
}
/* GCC calls libm's round and roundf for ties-away rounding (FCVTA*, FRINTA) unless told -fno-trapping-math, and a
   module links no libm. On the bits, with integers: Clang may not turn them back into a libm call */
double round(double x);
float roundf(float x);
double round(double x) {
    union {
        double d;
        u64 u;
    } v;
    int e;
    v.d = x;
    e = (int)((v.u >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x; /* whole already, or infinite or NaN */
    if (e < 0) {           /* under 1: 0 below a half, else 1, with the sign */
        v.u = (v.u & ((u64)1 << 63)) | (e == -1 ? (u64)0x3FF << 52 : 0);
        return v.d;
    }
    v.u += (u64)1 << (51 - e);          /* a half at this exponent, carrying into it */
    v.u &= ~(((u64)1 << (52 - e)) - 1); /* the fraction dropped */
    return v.d;
}
float roundf(float x) {
    union {
        float f;
        u32 u;
    } v;
    int e;
    v.f = x;
    e = (int)((v.u >> 23) & 0xFF) - 127;
    if (e >= 23) return x;
    if (e < 0) {
        v.u = (v.u & ((u32)1 << 31)) | (e == -1 ? (u32)0x7F << 23 : 0);
        return v.f;
    }
    v.u += (u32)1 << (22 - e);
    v.u &= ~(((u32)1 << (23 - e)) - 1);
    return v.f;
}
#ifdef _WIN32
/* GNU ld enters a DLL at DllMainCRTStartup, which the C runtime the gcc backend leaves out (-nostdlib) defines: without
   it the DLL would be entered at the start of its code. link_coff.c gives modules no entry */
int DllMainCRTStartup(void* module, unsigned long reason, void* reserved);
int DllMainCRTStartup(void* module, unsigned long reason, void* reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return 1;
}
#endif
