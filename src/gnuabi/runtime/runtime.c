/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "../process/process.h"
#include "../abi/errno/errno.h"
#include <stdio.h>
static void syscall_entry(CPU* c, u32 immediate) {
    GnuProcess* p = c->thread;
    if (immediate) {
        gnu_process_fault(p, 4, "Linux syscall entry requires SVC #0");
        return;
    }
    c->x[0] = (u64)gnu_syscall(p, c->x[8], c->x);
}
static void undefined(CPU* c, u64 pc, u32 instruction) {
    char reason[160];
    snprintf(reason, sizeof reason, "Unsupported instruction %08x at 0x%llx", instruction, (unsigned long long)pc);
    c->pc = pc;
    gnu_process_fault(c->thread, 4, reason);
}
static void breakpoint(CPU* c, u64 pc, u32 immediate) {
    char reason[160];
    snprintf(reason, sizeof reason, "Guest BRK #%u at 0x%llx", immediate, (unsigned long long)pc);
    c->pc = pc;
    gnu_process_fault(c->thread, 5, reason);
}
static void hint(u64 address) { (void)address; }
static u64 ticks(void) {
    int64_t sec, ns;
    if (gnu_clock_gettime(1, &sec, &ns)) return 0;
    return (u64)sec * 19200000 + (u64)ns * 192 / 10000;
}
static void fpcr(CPU* c) {
#if defined(__aarch64__)
    u64 value = c->fpcr & 0x07c00000;
    __asm__ __volatile__("msr fpcr, %0" ::"r"(value));
#elif defined(__x86_64__) || defined(_M_X64)
    u32 mx;
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    static const u32 modes[4] = {0, 0x4000, 0x2000, 0x6000};
    mx &= ~(0x8040u | 0x6000u);
    if (c->fpcr & (1u << 24)) mx |= 0x8040;
    mx |= modes[(c->fpcr >> 22) & 3];
    __asm__ __volatile__("ldmxcsr %0" ::"m"(mx));
#else
    (void)c;
#endif
}
static void event(void) {}
static void wait_event(CPU* c) { (void)c; }
const RtAPI gnu_rtapi = {syscall_entry, undefined, hint, ticks, breakpoint, fpcr, event, wait_event};
