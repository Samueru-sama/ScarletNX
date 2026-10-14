/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb_cpu.h"
#include "bios/bios.h"
#include "board/board.h"
#include <stdio.h>
#include <string.h>

/* The handler returns to this address: the machine takes the interrupt's epilogue from there. */
#define AGB_IRQ_RETURN             0x00000004u
/* Recompiled blocks are charged two cycles per instruction, the ARM7TDMI's average. */
#define AGB_CYCLES_PER_INSTRUCTION 2

typedef struct {
    AgbBoard* board;
    int16_t* audio;
    uint32_t capacity, written, elapsed; /* every slice of machine time, waits included */
} AgbSink;

static int bank_index(uint32_t cpsr) {
    switch (cpsr & 0x1F) {
        case 0x11: return 1; /* fiq */
        case 0x12: return 2; /* irq */
        case 0x13: return 3; /* svc */
        case 0x17: return 4; /* abt */
        case 0x1B: return 5; /* und */
        default: return 0;   /* usr and sys */
    }
}

/* The rotated immediate of the A32 data processing forms. */
static uint32_t expanded(uint32_t word) {
    unsigned rotate = (word >> 8 & 15) * 2;
    uint32_t value = word & 0xFF;
    return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

static void switch_mode(AgbCpu* cpu, uint32_t mode) {
    int old_mode = bank_index(cpu->arm.cpsr), new_mode = bank_index(mode);
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)0x1F) | (mode & 0x1F);
    if (old_mode == new_mode) return;
    cpu->banked_r13[old_mode] = cpu->arm.r[13];
    cpu->banked_r14[old_mode] = cpu->arm.r[14];
    if (old_mode == 1)
        for (int i = 0; i < 5; ++i) cpu->fiq_r8[i] = cpu->arm.r[8 + i];
    cpu->arm.r[13] = cpu->banked_r13[new_mode];
    cpu->arm.r[14] = cpu->banked_r14[new_mode];
    if (new_mode == 1)
        for (int i = 0; i < 5; ++i) cpu->arm.r[8 + i] = cpu->fiq_r8[i];
}

void agb_cpu_init(AgbCpu* cpu, uint8_t* map) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->io_window = map + 0x04000000;
    cpu->arm.mem = map; /* the flat window: one pointer add per guest access */
    cpu->arm.mem_size = AGB_MAP_SIZE;
    /* the boot state the BIOS leaves behind: system mode with the stacks set up */
    cpu->arm.cpsr = 0x1F;
    cpu->arm.r[13] = 0x03007F00;
    cpu->banked_r13[2] = 0x03007FA0;
    cpu->banked_r13[3] = 0x03007FE0;
    cpu->arm.r[15] = 0x080000C0;
}

static void advance(AgbCpu* cpu, AgbSink* sink, uint32_t cycles) {
    AgbBoard* board = sink->board;
    agb_ppu_step(board, cycles);
    agb_timer_step(board, cycles);
    if (sink->audio && sink->written < sink->capacity)
        sink->written += agb_apu_run(board, cycles, sink->audio + sink->written * 2, sink->capacity - sink->written);
    agb_io_sync(board, cpu->io_window);
    sink->elapsed += cycles;
}

/* ---- interrupts ---- */

static int irq_pending(AgbBoard* board) { return (board->irq_ime & 1) && (board->irq_ie & board->irq_if & 0x3FF); }

/* The interrupt entry the BIOS stub performs: the live registers land on the IRQ stack, the handler at
   0x03007FFC runs, and its return reaches the epilogue the machine services. Returns 0 while no handler
   is installed yet, so the caller keeps the machine moving. */
static int irq_enter(AgbBoard* board, AgbCpu* cpu) {
    uint32_t handler = agb_read32(board, 0x03007FFC);
    uint32_t saved_cpsr = cpu->arm.cpsr, next = cpu->arm.r[15], sp;
    if (!handler) return 0;
    /* the handler acknowledges its flags through the register window, but a write of the value already
       there never reaches the devices: the machine acknowledges what it took as well */
    cpu->irq_taken = (uint16_t)(board->irq_ie & board->irq_if);
    switch_mode(cpu, 0x12);
    cpu->spsr[2] = saved_cpsr;
    cpu->arm.r[14] = next + 4; /* hardware sets lr to the interrupted pc plus four */
    sp = cpu->arm.r[13] - 24;
    agb_write32(board, sp, cpu->arm.r[0]);
    agb_write32(board, sp + 4, cpu->arm.r[1]);
    agb_write32(board, sp + 8, cpu->arm.r[2]);
    agb_write32(board, sp + 12, cpu->arm.r[3]);
    agb_write32(board, sp + 16, cpu->arm.r[12]);
    agb_write32(board, sp + 20, cpu->arm.r[14]);
    cpu->arm.r[13] = sp;
    cpu->arm.r[14] = AGB_IRQ_RETURN;                               /* the handler returns to the epilogue */
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T) | 0x80; /* ARM state, interrupts off */
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T) | (handler & 1 ? SNX_ARM_T : 0);
    cpu->arm.r[15] = handler & ~(uint32_t)1;
    return 1;
}

/* The other half of the BIOS stub: the saved registers come back and the interrupted code resumes. */
static void irq_return(AgbBoard* board, AgbCpu* cpu) {
    uint32_t sp = cpu->arm.r[13], saved_cpsr = cpu->spsr[2], back;
    cpu->arm.r[0] = agb_read32(board, sp);
    cpu->arm.r[1] = agb_read32(board, sp + 4);
    cpu->arm.r[2] = agb_read32(board, sp + 8);
    cpu->arm.r[3] = agb_read32(board, sp + 12);
    cpu->arm.r[12] = agb_read32(board, sp + 16);
    back = agb_read32(board, sp + 20);
    cpu->arm.r[13] = sp + 24;
    board->irq_if &= (uint16_t)~cpu->irq_taken;
    switch_mode(cpu, saved_cpsr & 0x1F);
    cpu->arm.cpsr = saved_cpsr;
    cpu->arm.r[15] = back - 4;
}

/* ---- software interrupts and the privileged residue ---- */

static int handle_swi(AgbCpu* cpu, AgbSink* sink, uint32_t syndrome, int thumb) {
    AgbBoard* board = sink->board;
    uint32_t number = thumb ? syndrome & 0xFF : (syndrome >> 16) & 0xFF;
    if (number == AGB_SWI_HALT || number == AGB_SWI_STOP) {
        board->cpu_halted = 1;
        return 0;
    }
    if (number == AGB_SWI_INTR_WAIT || number == AGB_SWI_VBLANK_INTR_WAIT) {
        uint32_t mask = number == AGB_SWI_VBLANK_INTR_WAIT ? AGB_IRQ_VBLANK : (cpu->arm.r[1] & 0x3FF);
        /* the console waits forever here; the machine gives up after a second so a broken ROM cannot
           hang the emulator */
        uint32_t patience = AGB_SCANLINE_CYCLES * AGB_FRAME_LINES * 60;
        if (number == AGB_SWI_INTR_WAIT && cpu->arm.r[0]) board->irq_if &= (uint16_t)~mask;
        while (!(board->irq_if & mask) && patience) {
            advance(cpu, sink, 64);
            patience -= 64;
        }
        return 0;
    }
    return agb_bios_call(board, number, cpu->arm.r) < 0 ? -1 : 0;
}

/* The recompiler refuses the privileged instructions of the ARM7TDMI. The machine interprets them
   against the banked state and resumes at the next instruction. */
static int interpret(AgbBoard* board, AgbCpu* cpu, uint32_t word) {
    if ((word & 0x0fbf0fff) == 0x010f0000) { /* MRS Rd, CPSR or SPSR */
        unsigned rd = word >> 12 & 15;
        cpu->arm.r[rd] = word & (1u << 22) ? cpu->spsr[bank_index(cpu->arm.cpsr)] : cpu->arm.cpsr;
        cpu->arm.r[15] += 4;
        return 0;
    }
    if ((word & 0x0fb0fff0) == 0x0120f000 || (word & 0x0fb0f000) == 0x0320f000) { /* MSR */
        unsigned fields = word >> 16 & 15;
        uint32_t value = word & (1u << 25) ? expanded(word) : cpu->arm.r[word & 15];
        uint32_t bits = 0;
        if (fields & 8) bits |= 0xFF000000;
        if (fields & 4) bits |= 0x00FF0000;
        if (fields & 2) bits |= 0x0000FF00;
        if (fields & 1) bits |= 0x000000FF;
        if (word & (1u << 22)) {
            uint32_t* spsr = &cpu->spsr[bank_index(cpu->arm.cpsr)];
            *spsr = (*spsr & ~bits) | (value & bits);
        } else {
            uint32_t updated = (cpu->arm.cpsr & ~bits) | (value & bits);
            if ((updated & 0x1F) != (cpu->arm.cpsr & 0x1F)) switch_mode(cpu, updated & 0x1F);
            cpu->arm.cpsr = (updated & ~(uint32_t)SNX_ARM_T) | (cpu->arm.cpsr & SNX_ARM_T);
        }
        cpu->arm.r[15] += 4;
        return 0;
    }
    if ((word & 0x0fb00ff0) == 0x01000090) { /* SWP and SWPB */
        uint32_t address = cpu->arm.r[word >> 16 & 15], held = cpu->arm.r[word & 15];
        unsigned rd = word >> 12 & 15;
        if (word & (1u << 22)) {
            cpu->arm.r[rd] = agb_read8(board, address);
            agb_write8(board, address, (uint8_t)held);
        } else {
            cpu->arm.r[rd] = agb_read32(board, address);
            agb_write32(board, address, held);
        }
        cpu->arm.r[15] += 4;
        return 0;
    }
    /* exception return: a data processing write to the PC with the S bit set restores the SPSR */
    if ((word & 0x0c000000) == 0 && (word >> 12 & 15) == 15 && (word & (1u << 20))) {
        unsigned opcode = word >> 21 & 15;
        uint32_t operand = word & (1u << 25) ? expanded(word) : cpu->arm.r[word & 15];
        uint32_t result, saved = cpu->spsr[bank_index(cpu->arm.cpsr)];
        if (opcode == 13)
            result = operand; /* MOV */
        else if (opcode == 2)
            result = cpu->arm.r[word >> 16 & 15] - operand; /* SUB */
        else if (opcode == 4)
            result = cpu->arm.r[word >> 16 & 15] + operand; /* ADD */
        else
            return -1;
        switch_mode(cpu, saved & 0x1F);
        cpu->arm.cpsr = saved;
        cpu->arm.r[15] = result & ~(uint32_t)1;
        return 0;
    }
    return -1;
}

int agb_cpu_run(AgbBoard* board, AgbCpu* cpu, AgbRecomp* recomp, uint32_t cycles, int16_t* audio,
                uint32_t audio_capacity, uint32_t* audio_written, char* error, size_t error_size) {
    AgbSink sink = {board, audio, audio_capacity, 0, 0};
    while (sink.elapsed < cycles) {
        const AgbBlock* block;
        if (board->cpu_halted && (board->irq_if & board->irq_ie)) board->cpu_halted = 0;
        if (board->cpu_halted) {
            advance(cpu, &sink, 64);
            continue;
        }
        if (irq_pending(board) && !(cpu->arm.cpsr & 0x80)) {
            if (!irq_enter(board, cpu)) advance(cpu, &sink, 64);
            continue;
        }
        if (cpu->arm.r[15] == AGB_IRQ_RETURN) {
            irq_return(board, cpu);
            continue;
        }
        block = agb_recomp_lookup(recomp, board, cpu->arm.r[15], (cpu->arm.cpsr & SNX_ARM_T) != 0, error, error_size);
        if (!block) return -1;
        cpu->arm.exit = SNX_ARM_OK;
        block->fn(&cpu->arm);
        advance(cpu, &sink, (uint32_t)block->instructions * AGB_CYCLES_PER_INSTRUCTION);
        if (cpu->arm.exit == SNX_ARM_SVC) {
            if (handle_swi(cpu, &sink, cpu->arm.syndrome, block->thumb)) {
                snprintf(error, error_size, "unknown BIOS call 0x%02X at 0x%08X", cpu->arm.syndrome, cpu->arm.r[15]);
                return -1;
            }
        } else if (cpu->arm.exit == SNX_ARM_UNDEFINED) {
            if (interpret(board, cpu, cpu->arm.syndrome)) {
                snprintf(error, error_size, "unimplemented instruction 0x%08X at 0x%08X", cpu->arm.syndrome,
                         cpu->arm.r[15]);
                return -1;
            }
        } else if (cpu->arm.exit != SNX_ARM_OK) {
            snprintf(error, error_size, "the recompiled ROM stopped (exit %u) at 0x%08X", cpu->arm.exit,
                     cpu->arm.r[15]);
            return -1;
        }
    }
    if (audio_written) *audio_written = sink.written;
    return 0;
}
