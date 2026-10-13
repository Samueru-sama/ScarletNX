/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The part of every game module the backends don't generate per game: the helpers (cpu_helpers.c), the fallback
   interpreter, and the C library functions LLVM lowers memory
   intrinsics to. It is compiled once, when the recompiler is built, with the project's Clang and the flags
   toolchain.c gives generated C (-march=native: build == host == target), and linked into each
   module by link_elf.c / link_coff.c. The modules import nothing, so they need no dynamic symbol resolution */
#include "cpu.h"
#include "cpu_helpers.c"

GuestFn snx_interp_entry;

/* snx-rtlib-gen writes the interpreter module.c holds (snx_a64_interp) with external linkage, so the module IR can
   point MOD.interp at it, and the step function (snx_a64_interp step mode) */
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
