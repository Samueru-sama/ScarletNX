/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Shared AArch32 executor: runs A32 blocks and checks the DRY SVC surface
   zero-extends r0-r7 from 32-bit to 64-bit for the common HLE handler. */
#include "cpu/arm/exec.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t number, arguments[6];
    int seen;
} Svc;

static int syscall(void* context, uint32_t imm, uint64_t number, const uint64_t arguments[6]) {
    Svc* s = (Svc*)context;
    (void)imm;
    s->number = number;
    for (unsigned i = 0; i < 6; i++) s->arguments[i] = arguments[i];
    s->seen = 1;
    return 1; /* stop */
}

int main(void) {
    /* mov r0,#0x80000000; mov r1,#0xFFFFFFFF; mov r7,#7; svc #0 */
    static const uint32_t code[] = {0xE3A00102, 0xE3E01000, 0xE3A07007, 0xEF000000};
    unsigned char mem[0x10000];
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x1000, code, sizeof code);
    SnxArmCPU cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.mem = mem;
    cpu.mem_size = sizeof mem;
    cpu.cpsr = 0x10; /* USR, A32 */
    cpu.r[15] = 0x1000;
    SnxArmExecRange range = {0x1000, mem + 0x1000, sizeof code};
    SnxArmExec* exec = snx_arm_exec_new(NULL, 0);
    if (!exec) {
        fprintf(stderr, "FAIL: cannot create executor\n");
        return 1;
    }
    Svc svc = {0};
    int exit = snx_arm_exec_run(exec, &cpu, &range, 1, syscall, &svc);
    snx_arm_exec_free(exec);
    int failures = 0;
    if (!svc.seen) {
        fprintf(stderr, "FAIL: syscall not reached (exit %d)\n", exit);
        failures++;
    } else {
        /* r0=0x80000000 and r1=0xFFFFFFFF must arrive ZERO-extended, r7 as number. */
        if (svc.arguments[0] != 0x80000000ull) {
            fprintf(stderr, "FAIL: args[0]=%llx expected 0x80000000 (zero-extend)\n",
                    (unsigned long long)svc.arguments[0]);
            failures++;
        }
        if (svc.arguments[1] != 0xFFFFFFFFull) {
            fprintf(stderr, "FAIL: args[1]=%llx expected 0xffffffff (zero-extend)\n",
                    (unsigned long long)svc.arguments[1]);
            failures++;
        }
        if (svc.number != 7) {
            fprintf(stderr, "FAIL: number=%llu expected 7\n", (unsigned long long)svc.number);
            failures++;
        }
    }
    if (!failures) puts("A32 executor and DRY zero-extending SVC passed");
    return failures ? 1 : 0;
}
