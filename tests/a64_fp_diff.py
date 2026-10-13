# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Bit/FPSR/NZCV differential tests against Unicorn's QEMU-derived AArch64 model.

Run: python tests/a64_fp_diff.py [--cc clang] [--random 64]
Requires unicorn==2.1.4. No game data or full runtime build required.
"""

import argparse
import ctypes
import os
import random
import subprocess
import tempfile
from collections import Counter
from pathlib import Path

from unicorn import UC_ARCH_ARM64, UC_MODE_ARM, Uc
from unicorn.arm64_const import (
    UC_ARM64_REG_CPACR_EL1,
    UC_ARM64_REG_FPCR,
    UC_ARM64_REG_FPSR,
    UC_ARM64_REG_NZCV,
    UC_ARM64_REG_Q0,
    UC_ARM64_REG_Q1,
    UC_ARM64_REG_Q2,
)

ROOT = Path(__file__).resolve().parents[1]
A64 = ROOT / 'src/common/cpu/aarch64'
MASK64 = (1 << 64) - 1


def instructions():
    for size in (32, 64):
        for name, op in zip('NPMZAXI', (8, 9, 10, 11, 12, 14, 15)):
            yield f'FRINT{name}.{size}', 0x1E204020 | ((size == 64) << 22) | (op << 15), size, 1
        for signal in (0, 1):
            for zero in (0, 1):
                w = 0x1E202020 | ((size == 64) << 22) | (signal << 4) | (zero << 3)
                if not zero:
                    w |= 2 << 16
                yield f'FCMP{"E" if signal else ""}.{size}{".zero" if zero else ""}', w, size, 1
            for cond in (0, 1):  # EQ false and NE true with the initial NZCV
                w = 0x1E220425 | ((size == 64) << 22) | (signal << 4) | (cond << 12)
                yield f'FCCMP{"E" if signal else ""}.{size}.cond{cond}', w, size, 1
        for scalar, lanes in [(True, 1)] + [(False, n) for n in ((2, 4) if size == 32 else (2,))]:
            for name, u, hi, op in (
                ('FCMEQ', 0, 0, 0x1C),
                ('FCMGE', 1, 0, 0x1C),
                ('FCMGT', 1, 1, 0x1C),
                ('FACGE', 1, 0, 0x1D),
                ('FACGT', 1, 1, 0x1D),
            ):
                w = 0x0E200420 | (2 << 16) | ((scalar or lanes * size == 128) << 30) | (scalar << 28)
                w |= (u << 29) | (hi << 23) | ((size == 64) << 22) | (op << 11)
                yield f'{name}.{"scalar" if scalar else str(lanes) + "x"}{size}', w, size, lanes
            for name, u, op in (
                ('FCMEQ', 0, 0x0D),
                ('FCMGE', 1, 0x0C),
                ('FCMGT', 0, 0x0C),
                ('FCMLE', 1, 0x0D),
                ('FCMLT', 0, 0x0E),
            ):
                w = 0x0EA00820 | ((scalar or lanes * size == 128) << 30) | (scalar << 28)
                w |= (u << 29) | ((size == 64) << 22) | (op << 12)
                yield f'{name}.{"scalar" if scalar else str(lanes) + "x"}{size}.zero', w, size, lanes
        for op, name in enumerate(('FMAX', 'FMIN', 'FMAXNM', 'FMINNM'), 4):
            yield f'{name}.{size}', 0x1E220820 | ((size == 64) << 22) | (op << 12), size, 1
            w = 0x7E30C820 if op >= 6 else 0x7E30F820
            w |= ((size == 64) << 22) | ((op & 1) << 23)
            yield f'{name}P.scalar{size}', w, size, 128 // size
            if size == 32:
                yield f'{name}V.4x32', (w & ~(1 << 28)), size, 4
        for lanes in (2, 4) if size == 32 else (2,):
            for minimum in (0, 1):
                for numeric in (0, 1):
                    for pairwise in (0, 1):
                        name = f'F{"MIN" if minimum else "MAX"}{"NM" if numeric else ""}{"P" if pairwise else ""}'
                        w = 0x0E200420 | (2 << 16) | ((lanes * size == 128) << 30)
                        w |= (pairwise << 29) | (minimum << 23) | ((size == 64) << 22)
                        w |= (0x18 if numeric else 0x1E) << 11
                        yield f'{name}.{lanes}x{size}', w, size, lanes
            for name, k in zip('NPMZAXI', (0x18, 0x38, 0x19, 0x39, 0x58, 0x59, 0x79)):
                w = 0x0E200820 | ((lanes * size == 128) << 30) | ((size == 64) << 22)
                w |= ((k >> 6) << 29) | (((k >> 5) & 1) << 23) | ((k & 31) << 12)
                yield f'FRINT{name}.{lanes}x{size}', w, size, lanes


def values(size, count):
    frac, exp = (23, 8) if size == 32 else (52, 11)
    sign = 1 << (size - 1)
    inf = ((1 << exp) - 1) << frac
    bias = (1 << (exp - 1)) - 1
    # Both NaN kinds/signs/payloads, zero, subnormal boundaries, finite extremes,
    # ties and their adjacent representable numbers, integral precision limits.
    positive = [
        0,
        1,
        (1 << frac) - 1,
        1 << frac,
        inf - 1,
        inf,
        inf | 1,
        inf | (1 << (frac - 1)) | 0x123,
        inf | ((1 << frac) - 1),
    ]
    for exponent in (-1, 0, 1, frac - 1, frac):
        base = (bias + exponent) << frac
        positive.extend([base - 1, base, base + 1])
    positive.extend([(bias << frac) | (1 << (frac - 1)), ((bias + 1) << frac) | (1 << (frac - 2))])
    rng = random.Random(0xA64 + size)
    return sorted(set(positive + [x | sign for x in positive])) + [rng.getrandbits(size) for _ in range(count)]


def build(directory, cc, ops, sanitize=False):
    emitter = directory / 'emit'
    subprocess.run(
        [
            cc,
            '-O2',
            '-I',
            str(A64),
            '-I',
            str(ROOT / 'src/common/utilities'),
            str(ROOT / 'tests/a64_fp_emit.c'),
            str(A64 / 'a64.c'),
            '-o',
            str(emitter),
        ],
        check=True,
    )
    source = directory / 'generated.c'
    with source.open('w') as out:
        subprocess.run([str(emitter), *[f'{w:x}' for _, w, _, _ in ops]], stdout=out, check=True)
    library = directory / 'test.so'
    flags = ['-fsanitize=undefined', '-fsanitize-trap=undefined'] if sanitize else []
    subprocess.run(
        [
            cc,
            *flags,
            '-std=gnu11',
            '-O2',
            '-shared',
            '-fPIC',
            '-I',
            str(A64),
            str(source),
            '-o',
            str(library),
            '-lm',
            '-latomic',
        ],
        check=True,
    )
    lib = ctypes.CDLL(str(library))
    lib.run.argtypes = [ctypes.c_uint] * 6 + [ctypes.POINTER(ctypes.c_uint64)] * 2
    lib.run.restype = ctypes.c_int
    return lib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--random', type=int, default=64)
    parser.add_argument('--match', default='', help='only names containing this string')
    parser.add_argument('--aliases', action='store_true', help='also test Rd=Rn and Rd=Rm')
    parser.add_argument('--sanitize', action='store_true', help='trap on undefined behavior')
    args = parser.parse_args()
    ops = [op for op in instructions() if args.match in op[0]]
    if not ops:
        parser.error('--match selected no instructions')
    if args.aliases:
        for name, word, size, lanes in list(ops):
            if name.startswith(('FCMP', 'FCCMP')):
                continue
            ops.append((name + '/rd=rn', word & ~(31 << 5), size, lanes))
            if (word >> 16) & 31 == 2:
                ops.append((name + '/rd=rm', word & ~(31 << 16), size, lanes))
    arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    arm.mem_map(0x1000, 0x1000)
    arm.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    # Each opcode gets its own address, avoiding stale translated instruction caches.
    for index, (_, word, _, _) in enumerate(ops):
        arm.mem_write(0x1000 + index * 4, word.to_bytes(4, 'little'))
    failures, total = Counter(), 0
    samples = []
    with tempfile.TemporaryDirectory(prefix='snx-fp-') as directory:
        lib = build(Path(directory), args.cc, ops, args.sanitize)
        for index, (name, word, size, lanes) in enumerate(ops):
            corpus = values(size, args.random)
            for control in range(16):
                fpcr = ((control & 3) << 22) | (((control >> 2) & 1) << 24) | ((control >> 3) << 25)
                pairs = [(j, (j * 7 + 3) % len(corpus)) for j in range(len(corpus))]
                if not name.startswith('FRINT'):
                    # Cross every directed edge case with every other edge case,
                    # including both SNaN/QNaN orders and mixed signed zeros.
                    directed = len(values(size, 0))
                    pairs = [(j, k) for j in range(directed) for k in range(directed)] + pairs
                for j, k in pairs:
                    # Mix lanes to check exception accumulation, not just broadcast.
                    a = sum(corpus[(j + lane) % len(corpus)] << (size * lane) for lane in range(lanes))
                    a |= ((1 << 128) - 1) ^ ((1 << (size * lanes)) - 1)
                    b = sum(corpus[(k + lane) % len(corpus)] << (size * lane) for lane in range(lanes))
                    initial_fpsr, initial_nzcv = (1 << 27) | (2 if j & 1 else 0), 0xA0000000
                    r0 = a if (word >> 5) & 31 == 0 else b if (word >> 16) & 31 == 0 else (1 << 128) - 1
                    arm.reg_write(UC_ARM64_REG_Q0, r0)
                    arm.reg_write(UC_ARM64_REG_Q1, a)
                    arm.reg_write(UC_ARM64_REG_Q2, b)
                    arm.reg_write(UC_ARM64_REG_FPCR, fpcr)
                    arm.reg_write(UC_ARM64_REG_FPSR, initial_fpsr)
                    arm.reg_write(UC_ARM64_REG_NZCV, initial_nzcv)
                    pc = 0x1000 + index * 4
                    arm.emu_start(pc, pc + 4, count=1)
                    result = arm.reg_read(UC_ARM64_REG_Q0)
                    expected = (
                        result & MASK64,
                        result >> 64,
                        arm.reg_read(UC_ARM64_REG_FPSR),
                        arm.reg_read(UC_ARM64_REG_NZCV),
                    )
                    inputs = (ctypes.c_uint64 * 6)(r0 & MASK64, r0 >> 64, a & MASK64, a >> 64, b & MASK64, b >> 64)
                    for mode in (0, 1):
                        out = (ctypes.c_uint64 * 4)()
                        trap = lib.run(index, mode, word, fpcr, initial_fpsr, initial_nzcv, inputs, out)
                        total += 1
                        if trap or tuple(out) != expected:
                            failures[name] += 1
                            if len(samples) < 12:
                                samples.append(
                                    f'{name} {"interp" if mode else "AOT"} word={word:08x} '
                                    f'a={a:032x} b={b:032x} FPCR={fpcr:08x} FPSR={initial_fpsr:08x} '
                                    f'got={tuple(hex(x) for x in out)} expected={tuple(hex(x) for x in expected)} trap={trap}'
                                )
    for sample in samples:
        print(sample)
    print(
        f'{total} comparisons over {len(ops)} instruction cases, {sum(failures.values())} failures across {len(failures)} cases'
    )
    for name, count in failures.items():
        print(f'  {name}: {count}')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
