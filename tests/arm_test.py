#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Execute A32 LLVM objects at O0/O2. Optional independent Unicorn differential.

python tests/arm_test.py --emitter build/.../snx-test-arm-emit --cc clang [--unicorn]
No game assets. Unicorn is optional and pinned to 2.1.4 for reproducibility.
"""

import argparse
import ctypes as C
import pathlib
import random
import subprocess
import tempfile


class CPU(C.Structure):
    _fields_ = [
        ('r', C.c_uint32 * 16),
        ('cpsr', C.c_uint32),
        ('exit', C.c_uint32),
        ('syndrome', C.c_uint32),
        ('mem', C.POINTER(C.c_uint8)),
        ('mem_size', C.c_uint64),
        ('exclusive_value', C.c_uint64),
        ('exclusive_address', C.c_uint32),
        ('exclusive_size', C.c_uint32),
        ('exclusive_valid', C.c_uint32),
        ('tpidrurw', C.c_uint32),
        ('tpidruro', C.c_uint32),
        ('fpscr', C.c_uint32),
        ('d', C.c_uint64 * 32),
    ]


PC = 0x1000
SIZE = 0x10000
PROGRAMS = []
PCS = []
CASES = []


def program(*words, pc=PC):
    index = len(PROGRAMS)
    PROGRAMS.append(words)
    PCS.append(pc)
    return index


def case(word, group):
    CASES.append((program(word), group))


def catalogue():
    for op in range(16):
        for flags in (0, 1):
            if 8 <= op <= 11 and not flags:
                continue
            n = 0 if op in (13, 15) else 1
            d = 0 if 8 <= op <= 11 else 3
            base = 0xE0000000 | op << 21 | flags << 20 | n << 16 | d << 12
            for rotate in (0, 1, 8, 15):
                case(base | 1 << 25 | rotate << 8 | 0x81, 'data')
            for shift in range(4):
                for amount in (0, 1, 31):
                    case(base | amount << 7 | shift << 5 | 2, 'data')
                case(base | 4 << 8 | shift << 5 | 0x12, 'shift')
    for cond in range(15):
        case(cond << 28 | 0x02813001, 'condition')  # ADD r3,r1,#1
    # Every P/U/W/L/B/I combination, except invalid register offsets.
    for pre in (0, 1):
        for up in (0, 1):
            for wb in (0, 1):
                for load in (0, 1):
                    for byte in (0, 1):
                        base = 0xE4040000 | pre << 24 | up << 23 | byte << 22 | wb << 21 | load << 20
                        case(base | 13, 'memory')
                        for shift in range(4):
                            for amount in (0, 3):
                                case(base | 1 << 25 | amount << 7 | shift << 5 | 6, 'memory')
                    for op in (1, 2, 3):
                        if not pre and wb and not load and op >= 2:
                            continue
                        base = 0xE0040090 | pre << 24 | up << 23 | wb << 21 | load << 20 | op << 5
                        case(base | 1 << 22 | 8, 'extra')
                        case(base | 6, 'extra')
            for load in (0, 1):
                for wb in (0, 1):
                    case(0xE8040085 | pre << 24 | up << 23 | wb << 21 | load << 20, 'multiple')
    for word in (
        0xE3013234,
        0xE3453678,
        0xE16F3F12,  # MOVW/MOVT/CLZ
        0xE6BF3F32,
        0xE6BF3FB2,
        0xE6FF3F32,
        0xE6FF3FB2,  # REV/REV16/RBIT/REVSH
        0xE0030291,
        0xE0130291,
        0xE0234291,
        0xE0334291,
        0xE0634291,  # MUL/MLA/MLS
        0xE0834291,
        0xE0934291,
        0xE0A34291,
        0xE0B34291,
        0xE0C34291,
        0xE0D34291,
        0xE0E34291,
        0xE0F34291,  # long multiplies
        0xE713F211,
        0xE733F211,  # SDIV/UDIV
        0xE10F3000,
        0xE128F002,
        0xE124F002,
        0xE12CF002,
        0xE328F481,  # MRS/MSR
    ):
        case(word, 'misc')
    for op in (2, 3, 6, 7):
        for rotate in range(4):
            for n in (1, 15):
                case(0xE6803072 | op << 20 | n << 16 | rotate << 10, 'misc')
    for lsb, width in ((0, 32), (0, 1), (1, 31), (15, 8), (31, 1)):
        for base in (0xE7A03052, 0xE7E03052):
            case(base | (width - 1) << 16 | lsb << 7, 'misc')
        case(0xE7C03012 | (lsb + width - 1) << 16 | lsb << 7, 'misc')
        case(0xE7C0301F | (lsb + width - 1) << 16 | lsb << 7, 'misc')
    for word in (
        0xEA000002,
        0xEBFFFFFE,
        0x0A000001,
        0x1B000001,
        0xE12FFF12,
        0xE12FFF3E,
        0xFA000002,
        0xFBFFFFFE,
        0xE1A0F002,
        0xE28FF004,
    ):
        case(word, 'branch')
    for word in (0xE1940C9F, 0xE1D40C9F, 0xE1F40C9F, 0xE184FC90, 0xE1C4FC90, 0xE1E4FC90):
        case(word, 'atomic')
    for load, store in (
        (0xE1940F9F, 0xE1842F90),
        (0xE1D40F9F, 0xE1C42F90),
        (0xE1F40F9F, 0xE1E42F90),
        (0xE1B40F9F, 0xE1A42F90),
        (0xE1940E9F, 0xE1842E90),
        (0xE1D40E9F, 0xE1C42E90),
        (0xE1F40E9F, 0xE1E42E90),
        (0xE1B40E9F, 0xE1A42E90),
    ):
        CASES.append((program(load, 0xE2800001, store), 'atomic'))
        CASES.append((program(load, 0xF57FF01F, store), 'atomic'))
    for word in (0xE59F3004, 0xE1DF30B4, 0xE1CF00D8):
        # PC-relative word, halfword and doubleword literal loads.
        case(word, 'literal')


def state(words, seed=1, group='data'):
    rng = random.Random(seed)
    memory = (C.c_uint8 * SIZE).from_buffer_copy(rng.randbytes(SIZE))
    cpu = CPU()
    for i in range(15):
        cpu.r[i] = rng.getrandbits(32)
    edges = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678)
    if seed < 36:
        cpu.r[1] = edges[seed % len(edges)]
        cpu.r[2] = edges[(seed // len(edges)) % len(edges)]
    cpu.r[15] = PC
    cpu.cpsr = ((seed & 15) << 28) | 0x080A0010
    if group == 'shift':
        cpu.r[4] = (0, 1, 31, 32, 33, 63, 64, 255, 256, 257)[seed % 10]
    if group in ('memory', 'extra', 'multiple', 'atomic'):
        cpu.r[4] = 0x8000
        cpu.r[6] = 8
        cpu.r[13] = 0x8800
        cpu.cpsr &= ~(1 << 29)  # RRX address must stay within this small mapping
    if group == 'branch':
        cpu.r[2] = 0x2000 | (seed & 1)
        cpu.r[14] = 0x3000 | (seed & 1)
    cpu.mem = memory
    cpu.mem_size = SIZE
    for i, word in enumerate(words):
        memory[PC + 4 * i : PC + 4 * i + 4] = word.to_bytes(4, 'little')
    return cpu, memory


def compile_library(emitter, cc, directory, opt):
    words = directory / 'words.txt'
    words.write_text(''.join(f'{pc:x} ' + ' '.join(f'{w:08x}' for w in p) + '\n' for pc, p in zip(PCS, PROGRAMS)))
    obj = directory / f'arm{opt}.o'
    lib = directory / f'arm{opt}.so'
    subprocess.run([emitter, str(words), str(obj), str(opt)], check=True)
    subprocess.run([cc, '-shared', str(obj), '-o', str(lib)], check=True)
    handle = C.CDLL(str(lib))
    functions = []
    for i in range(len(PROGRAMS)):
        fn = getattr(handle, f'block_{i}')
        fn.argtypes = [C.POINTER(CPU)]
        fn.restype = None
        functions.append(fn)
    return functions


def witnesses():
    return {
        'base': program(0xE3A0002A, 0xE5840001, 0xE5941001, 0xE2812001),
        'predicated': program(0x05941000, 0x13A02007),
        'literal': program(0xE59F0004),
        'wrap': program(0xE5940020),
        'bounds': program(0xE5B40008),
        'ldrd': program(0xE1E400D8),
        'ldm': program(0xE8B40005),
        'pop_pc': program(0xE8BD8001),
        'str_pc': program(0xE584F000),
        'bx': program(0xE12FFF12),
        'blx_lr': program(0xE12FFF3E),
        'svc': program(0xEF001234),
        'bkpt': program(0xE1212374),
        'undefined': program(0xEE000910),  # unsupported coprocessor, not integer/VFP
        'reserved': program(0xF3B00D00),  # reserved VDUP lane, verified undefined
        'thumb16': program(0x00002000),
        'thumb32': program(0x0000F000),
        'monitor': program(0xE1940F9F, 0xE2800001, 0xE1841F90),
        'clrex': program(0xE1940F9F, 0xF57FF01F, 0xE1841F90),
        'strex': program(0xE1841F90),
        'exclusive_byte': program(0xE1D40F9F, 0xE1C41F90),
        'exclusive_half': program(0xE1F40F9F, 0xE1E41F90),
        'exclusive_double': program(0xE1B40F9F, 0xE1A42F90),
        'barriers': program(
            0xF57FF05B, 0xF57FF04F, 0xF57FF06F, 0xE320F000, 0xE320F001, 0xE320F002, 0xE320F003, 0xE320F004, 0xE320F005
        ),
        'bad_reg': program(0xE5B44004),
        'long': program(0xE0934291),
        'pc_alu': program(0xE1A0F002),
        'acquire': program(0xE1940C9F, 0xE2800001, 0xE184FC90),
        'acquire_byte': program(0xE1D40C9F, 0xE1C4FC90),
        'acquire_half': program(0xE1F40C9F, 0xE1E4FC90),
        'acquire_exclusive': program(0xE1940E9F, 0xE1841E90),
        'acquire_pair': program(0xE1B40E9F, 0xE1A42E90),
        'prefetch': program(0xF5D4F008, 0xF594F008, 0xF4D4F008, 0xF7D4F186),
        'tls': program(0xEE0D1F50, 0xEE1D2F50, 0xEE1D3F70),
        'loop_init': program(0xE3A00000, 0xE3A0100A),
        'loop_call': program(0xEB000006, pc=0x1008),
        'loop_test': program(0xE2511001, 0x1AFFFFFC, pc=0x100C),
        'loop_done': program(0xEF000001, pc=0x1014),
        'loop_func': program(0xE0800001, 0xE12FFF1E, pc=0x1028),
    }


def run_witnesses(functions, tests):
    def run(name, setup=None):
        idx = tests[name]
        cpu, mem = state(PROGRAMS[idx], group='memory')
        if setup:
            setup(cpu, mem)
        functions[idx](C.byref(cpu))
        return cpu, mem

    def word(mem, addr, value):
        mem[addr : addr + 4] = value.to_bytes(4, 'little')

    cpu, mem = run('base')
    assert (cpu.exit, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[15]) == (0, 42, 42, 43, PC + 16)
    assert bytes(mem[0x8001:0x8005]) == b'\x2a\0\0\0'
    cpu, _ = run('predicated', lambda c, m: c.r.__setitem__(4, SIZE + 100))
    assert cpu.exit == 0 and cpu.r[2] == 7
    cpu, _ = run('literal', lambda c, m: word(m, PC + 12, 0x87654321))
    assert cpu.exit == 0 and cpu.r[0] == 0x87654321
    cpu, _ = run('wrap', lambda c, m: (c.r.__setitem__(4, 0xFFFFFFF0), word(m, 0x10, 0x12345678)))
    assert cpu.exit == 0 and cpu.r[0] == 0x12345678
    cpu, _ = run('bounds', lambda c, m: c.r.__setitem__(4, SIZE - 4))
    assert cpu.exit == 6 and cpu.r[4] == SIZE - 4 and cpu.r[15] == PC
    cpu, _ = run('long', lambda c, m: (c.r.__setitem__(1, 0xFFFFFFFF), c.r.__setitem__(2, 0xFFFFFFFF)))
    assert cpu.exit == 0 and (cpu.r[3], cpu.r[4]) == (0xFFFFFFFE, 1) and (cpu.cpsr >> 30) == 2
    cpu, _ = run('ldm', lambda c, m: c.r.__setitem__(4, SIZE - 4))
    assert cpu.exit == 6 and cpu.r[4] == SIZE - 4 and cpu.r[15] == PC
    cpu, _ = run('ldrd', lambda c, m: (word(m, 0x8008, 0x12345678), word(m, 0x800C, 0xABCDEF01)))
    assert cpu.exit == 0 and (cpu.r[0], cpu.r[1], cpu.r[4]) == (0x12345678, 0xABCDEF01, 0x8008)
    cpu, _ = run('ldm', lambda c, m: (word(m, 0x8000, 7), word(m, 0x8004, 9)))
    assert cpu.exit == 0 and (cpu.r[0], cpu.r[2], cpu.r[4]) == (7, 9, 0x8008)
    cpu, _ = run('pop_pc', lambda c, m: (c.r.__setitem__(13, 0x8000), word(m, 0x8000, 42), word(m, 0x8004, 0x2001)))
    assert cpu.exit == 0 and (cpu.r[0], cpu.r[13], cpu.r[15], cpu.cpsr & 32) == (42, 0x8008, 0x2000, 32)
    cpu, mem = run('str_pc')
    assert cpu.exit == 0 and int.from_bytes(mem[0x8000:0x8004], 'little') == PC + 8
    for name in ('bx', 'pc_alu'):
        cpu, _ = run(name, lambda c, m: c.r.__setitem__(2, 0x2001))
        assert cpu.exit == 0 and cpu.r[15] == 0x2000 and cpu.cpsr & 32
        cpu, _ = run(name, lambda c, m: c.r.__setitem__(2, 0x2002))
        # BX to a misaligned ARM target completes (PC set); the alignment fault
        # is at the next fetch (the dispatcher's job), matching Unicorn.
        assert cpu.exit == 0 and cpu.r[15] == 0x2002 and not (cpu.cpsr & 32)
    cpu, _ = run('blx_lr', lambda c, m: c.r.__setitem__(14, 0x3001))
    assert cpu.exit == 0 and cpu.r[14] == PC + 4 and cpu.r[15] == 0x3000
    for name, status in (('svc', 4), ('bkpt', 5)):
        cpu, _ = run(name)
        assert cpu.exit == status and cpu.syndrome == 0x1234 and cpu.r[15] == PC + 4
    for name in ('undefined', 'reserved', 'bad_reg'):
        cpu, _ = run(name)
        assert cpu.exit == 1 and cpu.r[15] == PC
    for name in ('thumb16', 'thumb32'):
        cpu, _ = run(name, lambda c, m: setattr(c, 'cpsr', c.cpsr | 32))
        # An A32 block rejects Thumb state as a bad entry; Thumb is supported
        # via snx_thumb_llvm_block, so this is a wrong-block dispatch.
        assert cpu.exit == 8 and cpu.r[15] == PC
    cpu, mem = run('monitor', lambda c, m: word(m, 0x8000, 99))
    assert cpu.exit == 0 and cpu.r[1] == 0 and not cpu.exclusive_valid
    assert int.from_bytes(mem[0x8000:0x8004], 'little') == 100
    cpu, mem = run('clrex', lambda c, m: word(m, 0x8000, 99))
    assert cpu.exit == 0 and cpu.r[1] == 1 and not cpu.exclusive_valid
    assert int.from_bytes(mem[0x8000:0x8004], 'little') == 99
    cpu, _ = run('strex')
    assert cpu.exit == 0 and cpu.r[1] == 1
    for name in ('exclusive_byte', 'exclusive_half', 'exclusive_double', 'barriers'):
        cpu, _ = run(name)
        assert cpu.exit == 0, (name, cpu.exit)
        if name != 'barriers':
            assert cpu.r[2 if name == 'exclusive_double' else 1] == 0
    cpu, _ = run('exclusive_double', lambda c, m: c.r.__setitem__(4, 0x8004))
    assert cpu.exit == 7
    cpu, _ = run('base', lambda c, m: setattr(c, 'cpsr', c.cpsr | 512))
    assert cpu.exit == 9
    cpu, _ = run('base', lambda c, m: c.r.__setitem__(15, PC + 4))
    assert cpu.exit == 8
    cpu, mem = run('acquire', lambda c, m: word(m, 0x8000, 99))
    assert cpu.exit == 0 and cpu.r[0] == 100
    assert int.from_bytes(mem[0x8000:0x8004], 'little') == 100
    for name in ('acquire_byte', 'acquire_half', 'acquire_exclusive', 'acquire_pair'):
        cpu, _ = run(name)
        assert cpu.exit == 0
        if name in ('acquire_exclusive', 'acquire_pair'):
            assert cpu.r[2 if name == 'acquire_pair' else 1] == 0
    cpu, _ = run('prefetch', lambda c, m: c.r.__setitem__(4, SIZE + 64))
    assert cpu.exit == 0  # prefetch never faults
    cpu, _ = run('tls', lambda c, m: (c.r.__setitem__(1, 0x12345678), setattr(c, 'tpidruro', 0x87654321)))
    assert cpu.exit == 0 and (cpu.r[2], cpu.r[3], cpu.tpidrurw) == (0x12345678, 0x87654321, 0x12345678)
    # Dispatch a complete integer program across BL/BX and a conditional loop.
    dispatch = {PCS[i]: functions[i] for name, i in tests.items() if name.startswith('loop_')}
    cpu, mem = state(())
    for _ in range(64):
        dispatch[cpu.r[15]](C.byref(cpu))
        if cpu.exit:
            break
    assert cpu.exit == 4 and cpu.syndrome == 1 and cpu.r[0] == 55 and cpu.r[1] == 0
    print(f'  {len(tests)} witness programs passed')


def differential(functions):
    import unicorn as U
    from unicorn import arm_const as A

    regs = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC]
    emu = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_ARM)
    emu.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)  # includes ARMv8 AArch32 acquire/release
    emu.mem_map(0, SIZE)
    total = 0
    for index, group in CASES:
        words = PROGRAMS[index]
        for seed in range(64):
            cpu, memory = state(words, seed, group)
            emu.mem_write(0, bytes(memory))
            emu.ctl_remove_cache(PC, PC + 4 * len(words))
            emu.reg_write(A.UC_ARM_REG_CPSR, cpu.cpsr)
            for reg, value in zip(regs, cpu.r):
                emu.reg_write(reg, value)
            try:
                emu.emu_start(PC, SIZE, count=len(words))
            except U.UcError as exc:
                raise AssertionError(f'Unicorn rejected {words[0]:08x} ({group}): {exc}') from exc
            functions[index](C.byref(cpu))
            actual = list(cpu.r) + [cpu.cpsr]
            expected = [emu.reg_read(reg) for reg in regs] + [emu.reg_read(A.UC_ARM_REG_CPSR)]
            assert cpu.exit == 0, f'{words[0]:08x} ({group}) exit={cpu.exit} syndrome={cpu.syndrome:x} seed={seed}'
            assert actual == expected, f'{words[0]:08x} ({group}) seed={seed}: ' + ', '.join(
                f'r{i}: {x:08x} != {y:08x}' for i, (x, y) in enumerate(zip(actual, expected)) if x != y
            )
            assert bytes(memory) == bytes(emu.mem_read(0, SIZE)), f'{words[0]:08x}: memory mismatch'
            total += 1
    print(f'  {total} Unicorn differential executions passed ({len(CASES)} encodings)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emitter', required=True)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--unicorn', action='store_true')
    parser.add_argument('--opt', choices=('0', '2'), action='append')
    args = parser.parse_args()
    if args.unicorn:
        catalogue()
    tests = witnesses()
    with tempfile.TemporaryDirectory(prefix='snx-a32-') as directory:
        for opt in args.opt or ('0', '2'):
            print(f'A32 LLVM O{opt}', flush=True)
            functions = compile_library(args.emitter, args.cc, pathlib.Path(directory), opt)
            run_witnesses(functions, tests)
            if args.unicorn:
                differential(functions)


if __name__ == '__main__':
    main()
