/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Part of cpu_helpers.c, which includes it: bitmask immediates, CRC32, the 16-byte compare-and-swap, and the system registers (MRS, MSR). Not compiled on its own. */
/* ================================================================ integer */
u64 snx_decode_bitmask(u32 N, u32 imms, u32 immr, u32 ds) {
    u32 v = (N << 6) | (~imms & 0x3F);
    int len = -1;
    for (int i = 6; i >= 0; i--)
        if (v & (1u << i)) {
            len = i;
            break;
        }
    if (len < 1) return 0;
    u32 esize = 1u << len, levels = esize - 1;
    u32 S = imms & levels, R = immr & levels;
    if (S == levels) return 0;
    u64 welem = bmask(S + 1);
    if (R) welem = ((welem >> R) | (welem << (esize - R))) & bmask(esize);
    u64 r = 0;
    for (u32 i = 0; i < ds; i += esize) r |= welem << i;
    return ds == 32 ? (u32)r : r;
}

#if defined(__x86_64__) && defined(__SSE4_2__)
/* CRC32C is SSE4.2's CRC32 instruction */
static u32 crc32c_sse42(u32 acc, u64 val, int bytes) {
    switch (bytes) {
        case 1: return _mm_crc32_u8(acc, (u8)val);
        case 2: return _mm_crc32_u16(acc, (u16)val);
        case 4: return _mm_crc32_u32(acc, (u32)val);
        default: return (u32)_mm_crc32_u64(acc, val);
    }
}
#endif
#if defined(__x86_64__) && defined(__PCLMUL__)
static u64 clmul_lo(u64 a, u64 b) {
    return (u64)_mm_cvtsi128_si64(
        _mm_clmulepi64_si128(_mm_cvtsi64_si128((long long)a), _mm_cvtsi64_si128((long long)b), 0));
}
/* Barrett reduction, as the LLVM backend's crc32() in ir_a64.c */
static u32 crc32_clmul(u32 acc, u64 val, int bytes, int castagnoli) {
    u64 poly = castagnoli ? 0x105EC76F1ull : 0x1DB710641ull, mu = castagnoli ? 0xDEA713F1ull : 0x1F7011641ull;
    int steps = bytes == 8 ? 2 : 1, n = bytes == 8 ? 32 : 8 * bytes, k;
    for (k = 0; k < steps; k++) {
        u32 x = acc ^ (u32)(val >> (32 * k)), t;
        if (n < 32) x = (x & ((1u << n) - 1)) << (32 - n);
        t = (u32)(clmul_lo(clmul_lo(x, mu) & 0xFFFFFFFFu, poly) >> 32);
        acc = n < 32 ? t ^ (acc >> n) : t;
    }
    return acc;
}
#endif
/* a byte in two lookups instead of eight shifts */
static const u32 crc32_nibble[2][16] = {
    {0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C, 0xEDB88320,
     0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C},
    {0x00000000, 0x105EC76F, 0x20BD8EDE, 0x30E349B1, 0x417B1DBC, 0x5125DAD3, 0x61C69362, 0x7198540D, 0x82F63B78,
     0x92A8FC17, 0xA24BB5A6, 0xB21572C9, 0xC38D26C4, 0xD3D3E1AB, 0xE330A81A, 0xF36E6F75}};
u32 snx_crc32(u32 acc, u64 val, int bytes, int castagnoli) {
#if !(defined(__x86_64__) && defined(__PCLMUL__))
    const u32* t = crc32_nibble[castagnoli != 0];
#endif
#if defined(__x86_64__) && defined(__SSE4_2__)
    if (castagnoli) return crc32c_sse42(acc, val, bytes);
#endif
#if defined(__x86_64__) && defined(__PCLMUL__)
    return crc32_clmul(acc, val, bytes, castagnoli);
#else
    for (int i = 0; i < bytes; i++) {
        acc ^= (u8)(val >> (i * 8));
        acc = (acc >> 4) ^ t[acc & 15];
        acc = (acc >> 4) ^ t[acc & 15];
    }
    return acc;
#endif
}

/* the 16-byte builtin compiles to lock cmpxchg16b only where cx16 is enabled, and the game module isn't built with
   -mcx16, so STXP of a 64-bit pair used to take a fallback that swapped the two halves separately. That isn't atomic:
   Tears of the Kingdom's lock-free lists (102 such pairs, a pointer and a counter) got a pointer from one thread and a
   counter from another. Every x86-64 CPU that runs the game has cmpxchg16b, so this function enables it for itself.
   Clang still warns that 16 bytes is past its lock-free size, though it emits the one instruction */
#ifndef __clang__
#error "game modules are built with Clang: GCC makes the 16-byte compare-and-swap a call into libatomic"
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Watomic-alignment"
#if defined(__x86_64__)
__attribute__((target("cx16")))
#endif
u32 snx_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1) {
    unsigned __int128 o = ((unsigned __int128)o1 << 64) | o0, n = ((unsigned __int128)n1 << 64) | n0;
    return __atomic_compare_exchange_n((unsigned __int128*)p, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#pragma clang diagnostic pop

/* ================================================================ system */
#define SYSREG(op0, op1, crn, crm, op2) ((((op0) & 1) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
u64 snx_mrs(CPU* c, u32 r) {
    switch (r) {
        case SYSREG(3, 3, 13, 0, 2): return c->tpidr;
        case SYSREG(3, 3, 13, 0, 3): return c->tpidrro;
        case SYSREG(3, 3, 4, 2, 0): return get_nzcv(c);
        case SYSREG(3, 3, 4, 4, 0): return c->fpcr;
        case SYSREG(3, 3, 4, 4, 1): return c->fpsr;
        case SYSREG(3, 3, 14, 0, 0): return 19200000;
        case SYSREG(3, 3, 14, 0, 1):
        case SYSREG(3, 3, 14, 0, 2): return c->rt->ticks();
        case SYSREG(3, 3, 0, 0, 7): return 4;           /* DCZID = 64 bytes */
        case SYSREG(3, 3, 0, 0, 1): return 0x8444C004u; /* CTR_EL0 (A57) */
        case SYSREG(3, 0, 0, 0, 0): return 0x411FD071u; /* MIDR */
        case SYSREG(3, 0, 0, 0, 5): return 0x80000000u; /* MPIDR */
        default: return 0;
    }
}
void snx_msr(CPU* c, u32 r, u64 v) {
    switch (r) {
        case SYSREG(3, 3, 13, 0, 2): c->tpidr = v; break;
        case SYSREG(3, 3, 4, 2, 0): set_nzcv(c, (u32)(v >> 28)); break;
        case SYSREG(3, 3, 4, 4, 0):
            /* A57 TRM Tables 14-3/14-4: exception enables and FZ16 are RES0. */
            c->fpcr = (u32)v & 0x07C00000u;
            if (c->rt->fpcr_changed) c->rt->fpcr_changed(c);
            break;
        case SYSREG(3, 3, 4, 4, 1): c->fpsr = (u32)v & 0xF800009Fu; break;
        default: break;
    }
}
