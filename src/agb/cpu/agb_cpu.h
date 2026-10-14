/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The machine loop: recompiled blocks run on the flat map, software interrupts go to the HLE BIOS, and the
   privileged instructions the ARM recompiler refuses are interpreted here. */
#ifndef SNX_AGB_CPU_H
#define SNX_AGB_CPU_H
#include "cpu/arm/arm.h"
#include "recomp.h"
#include <stddef.h>
#include <stdint.h>

typedef struct AgbBoard AgbBoard;
typedef struct {
    SnxArmCPU arm;
    /* the banked registers of the ARM7TDMI's modes: usr and sys share one bank */
    uint32_t banked_r13[6], banked_r14[6], spsr[6];
    uint32_t fiq_r8[5];
    uint16_t irq_taken; /* the flags this interrupt was taken for */
    uint8_t* io_window; /* the register window inside the flat map */
} AgbCpu;

void agb_cpu_init(AgbCpu* cpu, uint8_t* map);
/* Runs the board for the given number of CPU cycles. audio collects interleaved stereo samples at
   AGB_APU_RATE, up to capacity. Returns 0, or -1 with a message when the machine stops. */
int agb_cpu_run(AgbBoard* board, AgbCpu* cpu, AgbRecomp* recomp, uint32_t cycles, int16_t* audio,
                uint32_t audio_capacity, uint32_t* audio_written, char* error, size_t error_size);
#endif
