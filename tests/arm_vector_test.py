#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A32 VFP/NEON LLVM execution vs ARMv8 AArch32 (Unicorn 2.1.4).

Assemble independent mnemonics with llvm-mc -mcpu=cortex-a57. Verify and compile
LLVM at O0 and O2, then compare every GPR, D register, CPSR, FPSCR and memory.
No host floating-point comparisons or tolerances. No skipped mismatches.
"""

import argparse
import ctypes as C
import pathlib
import random
import re
import subprocess
import tempfile

from arm_test import CPU, PC, SIZE


def catalogue():
    here = pathlib.Path(__file__).parent
    instructions = []
    for name in ('arm_vfp_cases.s', 'arm_neon_cases.s'):
        for line in (here / name).read_text().splitlines():
            line = line.strip()
            if not line or line[0] in './*' or line.startswith('Copyright'):
                continue
            instructions.append(line)
    # Register aliases and D16..D31 must not be confused with S32..S63.
    originals = list(instructions)
    for text in originals:
        if '{' not in text and '[' not in text and text.startswith(('v', 'aes', 'sha')):

            def high(match):
                kind, number = match[1], int(match[2])
                if kind == 's':
                    return f's{(number + 16) % 32}'
                if kind == 'q':
                    return f'q{(number + 8) % 16}'
                return f'd{(number + 16) % 32}'

            # By-element operands have restricted register ranges, leave them alone.
            instructions.append(re.sub(r'(?<![.\w])([sdq])(\d+)\b', high, text))
        operands = re.findall(r'\b([dq]\d+)\b', text)
        if (
            len(operands) in (2, 3)
            and '{' not in text
            and '[' not in text
            and not text.startswith(('vtrn', 'vzip', 'vuzp', 'vswp'))
        ):
            dest, source = operands[0], operands[-1]
            if dest[0] == source[0]:
                instructions.append(re.sub(r'\b' + dest + r'\b', source, text, count=1))
    for op in ('vadd', 'vsub', 'vqadd', 'vqsub', 'vshl', 'vrshl', 'vqshl', 'vqrshl'):
        for size in (8, 16, 32, 64):
            for sign in ('i',) if op in ('vadd', 'vsub') else ('s', 'u'):
                for reg in ('d', 'q'):
                    instructions.append(f'{op}.{sign}{size} {reg}0,{reg}1,{reg}2')
    for op in ('vshr', 'vrshr', 'vsra', 'vrsra', 'vshl', 'vqshl', 'vqshlu', 'vsli', 'vsri'):
        for size in (8, 16, 32, 64):
            left = op in ('vshl', 'vqshl', 'vqshlu', 'vsli')
            for amount in (0, size - 1) if left else (1, size):
                signs = ('s',) if op == 'vqshlu' else ('i',) if op in ('vshl', 'vsli', 'vsri') else ('s', 'u')
                for sign in signs:
                    instructions.append(f'{op}.{sign}{size} q0,q2,#{amount}')
    # Single, multiple, structure, lane, replicate and post-indexed memory forms.
    for load in ('vld', 'vst'):
        for count in range(1, 5):
            lists = [[4 + i for i in range(count)]]
            if count == 1:
                lists = [[4 + i for i in range(n)] for n in range(1, 5)]
            else:
                lists.append([4 + i * 2 for i in range(count)])
                if count == 2:
                    lists.append([4, 5, 6, 7])
            for regs in lists:
                for width in (8, 16, 32, 64) if count == 1 else (8, 16, 32):
                    names = ','.join(f'd{r}' for r in regs)
                    for post in ('', '!', ',r6'):
                        for alignment in ('', ':64'):
                            instructions.append(f'{load}{count}.{width} {{{names}}},[r4{alignment}]{post}')
            for width in (8, 16, 32):
                for stride in (1,) if count == 1 or width == 8 else (1, 2):
                    names = ','.join(f'd{4 + i * stride}[{64 // width - 1}]' for i in range(count))
                    for post in ('', '!', ',r6'):
                        instructions.append(f'{load}{count}.{width} {{{names}}},[r4]{post}')
        if load == 'vld':
            for count in range(1, 5):
                for width in (8, 16, 32):
                    for stride in (1, 2):
                        names = ','.join(f'd{4 + i * stride}[]' for i in range(count))
                        for post in ('', '!', ',r6'):
                            instructions.append(f'vld{count}.{width} {{{names}}},[r4]{post}')
            instructions += ['vld1.16 {d4[],d5[]},[r4]!', 'vld4.32 {d4[],d6[],d8[],d10[]},[r4:128]!']
    return list(dict.fromkeys(instructions))


def assemble(mc, instructions):
    text = '.syntax unified\n.arm\n' + '\n'.join(instructions) + '\n'
    process = subprocess.run(
        [mc, '-triple=armv8-linux-gnueabi', '-mcpu=cortex-a57', '-show-encoding'],
        input=text,
        text=True,
        capture_output=True,
    )
    if process.returncode:
        raise RuntimeError(process.stderr)
    output = process.stdout
    words = []
    for line in output.splitlines():
        if 'encoding:' in line:
            data = re.findall(r'0x[0-9a-f]+', line.split('encoding:')[1])
            words.append(int.from_bytes(bytes(int(v, 16) for v in data), 'little'))
    assert len(words) == len(instructions)
    return words


EDGE32 = (
    0,
    0x80000000,
    1,
    0x007FFFFF,
    0x00800000,
    0x3F800001,
    0x3F800000,
    0x3F000000,
    0x7F800000,
    0xFF800000,
    0x7FC12345,
    0x7F812345,
    0xBF800000,
    0x7F7FFFFF,
    0x4EFFFFFF,
    0xCF000000,
)
EDGE64 = (
    0,
    0x8000000000000000,
    1,
    0xFFFFFFFFFFFFF,
    0x10000000000000,
    0x3FF0000000000001,
    0x3FF0000000000000,
    0x3FE0000000000000,
    0x7FF0000000000000,
    0xFFF0000000000000,
    0x7FF8123456789ABC,
    0x7FF0123456789ABC,
    0xBFF0000000000000,
    0x7FEFFFFFFFFFFFFF,
    0x41DFFFFFFFC00000,
    0xC1E0000000000000,
)


def initial(word, seed):
    rng = random.Random(seed)
    cpu = CPU()
    mem = (C.c_uint8 * SIZE).from_buffer_copy(rng.randbytes(SIZE))
    cpu.mem = mem
    cpu.mem_size = SIZE
    cpu.cpsr = (seed & 15) << 28 | 0x080A0010
    cpu.fpscr = (seed % 4) << 22 | ((seed // 4) % 16) << 24 | ((seed // 8) % 16) << 28
    if seed & 64:
        cpu.fpscr |= 0x9F  # cumulative exceptions must not be cleared
    for i in range(16):
        cpu.r[i] = 0x8000 + i * 16
    cpu.r[15] = PC
    for i in range(32):
        if seed < 32:
            cpu.d[i] = EDGE32[(seed + i) % 16] | EDGE32[(seed * 3 + i) % 16] << 32
        elif seed < 48:
            cpu.d[i] = EDGE64[(seed + i) % 16]
        elif seed < 64:
            # Identical byte/halfword/word signed endpoints, variable shifts and saturation.
            byte = (0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 129, 192, 224, 255)[(seed + i) % 16]
            cpu.d[i] = byte * 0x0101010101010101
        else:
            cpu.d[i] = rng.getrandbits(64)
    mem[PC : PC + 4] = word.to_bytes(4, 'little')
    return cpu, mem


def compare(functions, words, names, states):
    import unicorn as U
    from unicorn import arm_const as A

    u = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_ARM)
    u.ctl_set_cpu_model(A.UC_CPU_ARM_MAX)
    u.mem_map(0, SIZE)
    regs = [getattr(A, f'UC_ARM_REG_R{i}') for i in range(13)] + [A.UC_ARM_REG_SP, A.UC_ARM_REG_LR, A.UC_ARM_REG_PC]
    total = 0
    for index, (word, text) in enumerate(zip(words, names)):
        fn = functions[index]
        for seed in range(states):
            cpu, mem = initial(word, seed)
            u.mem_write(0, bytes(mem))
            u.ctl_remove_cache(PC, PC + 4)
            u.reg_write(A.UC_ARM_REG_CPSR, cpu.cpsr)
            u.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
            u.reg_write(A.UC_ARM_REG_FPSCR, cpu.fpscr)
            for r, value in zip(regs, cpu.r):
                u.reg_write(r, value)
            for i, value in enumerate(cpu.d):
                u.reg_write(A.UC_ARM_REG_D0 + i, value)
            u.emu_start(PC, SIZE, count=1)
            fn(C.byref(cpu))
            actual = list(cpu.r) + list(cpu.d) + [cpu.cpsr, cpu.fpscr]
            expected = [u.reg_read(r) for r in regs] + [u.reg_read(A.UC_ARM_REG_D0 + i) for i in range(32)]
            expected += [u.reg_read(A.UC_ARM_REG_CPSR), u.reg_read(A.UC_ARM_REG_FPSCR)]
            label = f'{word:08x} {text} seed={seed}'
            assert cpu.exit == 0, f'{label} exit={cpu.exit}'
            assert actual == expected, (
                label
                + ': '
                + ', '.join(
                    f'field{i}: {x:016x} != {y:016x}' for i, (x, y) in enumerate(zip(actual, expected)) if x != y
                )
            )
            assert bytes(mem) == bytes(u.mem_read(0, SIZE)), label + ': memory differs'
            total += 1
    print(f'  {total} exact comparisons passed ({len(words)} encodings)')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--emitter', required=True)
    p.add_argument('--runtime', required=True, help='libsnx_arm_rt.a')
    p.add_argument('--mc', required=True)
    p.add_argument('--cc', default='clang')
    p.add_argument('--states', type=int, default=80)
    p.add_argument('--opt', choices=('0', '2'), action='append')
    args = p.parse_args()
    names = catalogue()
    words = assemble(args.mc, names)
    with tempfile.TemporaryDirectory(prefix='snx-a32-vector-') as path:
        directory = pathlib.Path(path)
        source = directory / 'words.txt'
        source.write_text(''.join(f'{PC:x} {word:x}\n' for word in words))
        for opt in args.opt or ('0', '2'):
            print(f'A32 VFP/NEON LLVM O{opt}', flush=True)
            obj, dll = directory / f'test{opt}.o', directory / f'test{opt}.so'
            subprocess.run([args.emitter, str(source), str(obj), opt], check=True)
            subprocess.run([args.cc, '-shared', str(obj), args.runtime, '-o', str(dll)], check=True)
            lib = C.CDLL(str(dll))
            functions = [getattr(lib, f'block_{i}') for i in range(len(words))]
            for fn in functions:
                fn.argtypes = [C.POINTER(CPU)]
                fn.restype = None
            compare(functions, words, names, args.states)


if __name__ == '__main__':
    main()
