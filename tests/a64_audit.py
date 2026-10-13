# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A57 instruction audit, not a certification suite.

Uses the real decoder inventory and both emitters. Unicorn is explicitly set to
Cortex-A57; capstone supplies diagnostic mnemonics, not allocation decisions.
Reports counterexamples rather than hiding them behind expected-failure lists.
See docs/DIVERGENCES.md for architectural and oracle limitations.
Requires unicorn==2.1.4, capstone==5.0.7 and a native Linux Clang toolchain.
"""

import argparse
import ctypes as ct
import itertools
import json
import os
import random
import re
import subprocess
import tempfile
from pathlib import Path

import capstone
from unicorn import UC_ARCH_ARM64, UC_MODE_ARM, Uc, UcError
from unicorn import arm64_const as reg

ROOT = Path(__file__).resolve().parents[1]
A64 = ROOT / 'src/common/cpu/aarch64'
MASK = (1 << 64) - 1
PC, MEM_SIZE = 0x1000, 0x20000
XREGS = [getattr(reg, f'UC_ARM64_REG_X{i}') for i in range(31)] + [reg.UC_ARM64_REG_SP]
VREGS = [getattr(reg, f'UC_ARM64_REG_Q{i}') for i in range(32)]
STATE = ct.c_uint64 * 100
EDGES = [
    0,
    1,
    MASK,
    1 << 63,
    (1 << 63) - 1,
    0x80000000,
    0x7FFFFFFF,
    0xFFFFFFFF,
    0x5555555555555555,
    0xAAAAAAAAAAAAAAAA,
    8,
    16,
    32,
    64,
    127,
    128,
]
F32 = [
    0,
    0x80000000,
    1,
    0x80000001,
    0x007FFFFF,
    0x00800000,
    0x3F000000,
    0xBF000000,
    0x3FC00000,
    0x4F000001,
    0x7F7FFFFF,
    0x7F800000,
    0xFF800000,
    0x7FC00123,
    0x7F800456,
    0xFFC00789,
]
F64 = [
    0,
    1 << 63,
    1,
    (1 << 63) | 1,
    0x000FFFFFFFFFFFFF,
    0x0010000000000000,
    0x3FE0000000000000,
    0xBFE0000000000000,
    0x3FF8000000000000,
    0x43E0000000000001,
    0x7FEFFFFFFFFFFFFF,
    0x7FF0000000000000,
    0xFFF0000000000000,
    0x7FF8000000000123,
    0x7FF0000000000456,
    0xFFF8000000000789,
]
SYSTEM = {'SVC', 'BRK', 'HINT', 'MSR_IMM', 'SYS', 'MRS', 'MSR'}
MEMORY = {'LDST_UIMM', 'LDST_IMM9', 'LDST_REG', 'LDR_LIT', 'LDSTP', 'LDSTX', 'SIMD_LDST', 'SIMD_LDST1'}


def inventory(emitter):
    rows = []
    for line in subprocess.check_output([str(emitter), '--inventory'], text=True).splitlines():
        name, mask, value, count, *fields = line.split()
        rows.append((name, int(mask, 16), int(value, 16), int(count), [tuple(map(int, f.split(':'))) for f in fields]))
    return rows


def corpus(rows):
    words = {}

    def add(group, word):
        words.setdefault(word, group)

    for name, mask, value, count, fields in rows:
        if name in SYSTEM or name.startswith('SIMD_'):
            continue
        fieldmask = sum(((1 << width) - 1) << lo for lo, width in fields)
        for index in range(count):
            word, rest = value, index
            for lo, width in reversed(fields):
                word |= (rest & ((1 << width) - 1)) << lo
                rest >>= width
            free = ~(mask | fieldmask) & 0xFFFFFFFF
            word |= ((1 << 5) | (2 << 16)) & free
            if name in {'DP3', 'FP3', 'LDSTP'}:
                word |= (3 << 10) & free
                if name == 'DP3' and ((word >> 21) & 7) in (2, 6):
                    word |= 31 << 10  # SMULH/UMULH's SBO Ra field
            if name in {'B', 'BL'}:
                word = value | 4
            elif name in {'BCOND', 'CBZ', 'TBZ', 'LDR_LIT'}:
                word = (word & ~(0x3FFF << 5)) | (4 << 5)
            elif name == 'LDSTX':
                word |= 1 << 15
                word = (word & ~(31 << 10)) | ((3 if word & (1 << 21) else 31) << 10)
                if word & ((1 << 22) | (1 << 23)):
                    word |= 31 << 16
            elif name == 'LDST_REG':
                word |= 3 << 13
            elif name == 'BITFIELD' or name == 'EXTR':
                word |= ((word >> 31) & 1) << 22
            add(name, word)
            if name == 'LDSTX' and not (word & (1 << 23)):
                add(name, word & ~(1 << 15))  # non-acquire/release exclusives
            if name in {'FCMP', 'FCCMP'}:
                add(name, word | (1 << 4))  # signalling comparison variants
            if name == 'BITFIELD':
                for immr, imms in ((0, 7), (0, 15), (0, 31), (8, 15), (31, 63)):
                    add(name, (word & ~0x003FFC00) | (immr << 16) | (imms << 10))
            # Operand-field boundaries, XZR/SP, shift/immediate sign boundaries.
            if name in {
                'ADDSUB_IMM',
                'LOGIC_IMM',
                'MOVW',
                'BITFIELD',
                'EXTR',
                'LOGIC_REG',
                'ADDSUB_SREG',
                'ADDSUB_EXT',
                'ADC',
                'CCMP',
                'CSEL',
                'DP1',
                'DP2',
                'DP3',
            }:
                for lo, width in ((0, 5), (5, 5), (10, 6), (16, 5), (22, 2)):
                    fm = ((1 << width) - 1) << lo & free
                    add(name, word | fm)
            if name in {'ADR', 'ADRP'}:
                add(name, word | (1 << 23))
            if name in {'LDST_UIMM', 'LDST_IMM9', 'LDST_REG', 'LDSTP', 'LDSTX'}:
                add(name, (word & ~(31 << 5)) | (31 << 5))
            if name in {'LDST_IMM9', 'LDSTP'}:
                add(name, word | (1 << (20 if name == 'LDST_IMM9' else 21)))
    # Exhaust the opcode/size/Q/U dispatch dimensions, including unallocated cells.
    for scalar, q, u, size, op in itertools.product(range(2), range(2), range(2), range(4), range(32)):
        if scalar and not q:
            continue
        base = (q << 30) | (u << 29) | (scalar << 28) | (size << 22) | (1 << 5)
        add('SIMD_3same', 0x0E200400 | base | (2 << 16) | (op << 11))
        add('SIMD_3diff', 0x0E200000 | base | (2 << 16) | ((op & 15) << 12))
        add('SIMD_2misc', 0x0E200800 | base | (op << 12))
        add('SIMD_pairwise' if scalar else 'SIMD_across', 0x0E300800 | base | (op << 12))
    for scalar, q, u, op, immh, immb in itertools.product(
        range(2), range(2), range(2), range(32), (1, 2, 4, 8, 15), (0, 7)
    ):
        if scalar and not q:
            continue
        add(
            'SIMD_shimm',
            0x0F000400 | (q << 30) | (u << 29) | (scalar << 28) | (immh << 19) | (immb << 16) | (op << 11) | 32,
        )
    for q, u, size, op in itertools.product(range(2), range(2), range(4), range(16)):
        for scalar in range(2):
            if scalar and not q:
                continue
            add(
                'SIMD_elem',
                0x0F000000 | (q << 30) | (u << 29) | (scalar << 28) | (size << 22) | (2 << 16) | (op << 12) | 32,
            )
    for q, u, imm5, imm4 in itertools.product(range(2), range(2), (0, 1, 2, 4, 8, 16, 31), range(16)):
        add('SIMD_copy', 0x0E000400 | (q << 30) | (u << 29) | (imm5 << 16) | (imm4 << 11) | 32)
    for imm5 in range(32):
        add('SIMD_scalar_dup', 0x5E000420 | (imm5 << 16))
    for q, op, cmode, imm8 in itertools.product(range(2), range(2), range(16), (0, 0x55, 0x80, 0xFF)):
        add(
            'SIMD_modimm',
            0x0F000400 | (q << 30) | (op << 29) | (cmode << 12) | ((imm8 >> 5) << 16) | ((imm8 & 31) << 5),
        )
    for q, op, size in itertools.product(range(2), range(8), range(4)):
        add('SIMD_perm', 0x0E000800 | (q << 30) | (size << 22) | (2 << 16) | (op << 12) | 32)
    for q, length, extension in itertools.product(range(2), range(4), range(2)):
        add('SIMD_table', 0x0E000000 | (q << 30) | (2 << 16) | (length << 13) | (extension << 12) | 32)
    for q, offset in itertools.product(range(2), range(16)):
        add('SIMD_ext', 0x2E000000 | (q << 30) | (2 << 16) | (offset << 11) | 32)
    for q, load, post, op, size in itertools.product(range(2), range(2), range(2), range(16), range(4)):
        add(
            'SIMD_LDST',
            0x0C000000
            | (q << 30)
            | (load << 22)
            | (post << 23)
            | ((31 if post else 0) << 16)
            | (op << 12)
            | (size << 10)
            | 32,
        )
    for q, load, post, r, op, s, size in itertools.product(
        range(2), range(2), range(2), range(2), range(8), range(2), range(4)
    ):
        add(
            'SIMD_LDST1',
            0x0D000000
            | (q << 30)
            | (load << 22)
            | (post << 23)
            | (r << 21)
            | ((31 if post else 0) << 16)
            | (op << 13)
            | (s << 12)
            | (size << 10)
            | 32,
        )
    for op in range(8):
        add('optional_crypto', 0x4E280800 | (op << 12) | 32)
        add('optional_crypto', 0x5E000000 | (2 << 16) | (op << 12) | 32)
        add('optional_crypto', 0x5E280800 | (op << 12) | 32)
    # Cortex-A57 must not acquire later ISA extensions through permissive masks.
    for w in (
        0x889F7C20,
        0xC8DF7C20,
        0x88A07C22,
        0xB8208022,
        0xF8BFC020,
        0x1E284020,
        0x1EE02820,
        0x9AC23020,
        0xCE628C20,
    ):
        add('post_v8_0', w)
    for word, family in list(words.items()):
        if family in {'SIMD_LDST', 'SIMD_LDST1'}:
            add(family, (word & ~31) | 31)  # structure register lists wrap V31 -> V0
            if not (word & (1 << 23)):
                add(family, word | (2 << 16))  # no-offset forms require Rm=00000
            else:
                add(family, (word & ~(31 << 16)) | (2 << 16))  # register writeback
        if family == 'SIMD_LDST':
            add(family, word | (1 << 21))  # reserved in multiple-structure forms
    return [(family, word) for word, family in words.items()]


def state_for(word, family, iteration):
    rng = random.Random(0xA57 + iteration)
    state = [rng.getrandbits(64) for _ in range(100)]
    state[:4] = [
        EDGES[(iteration + 3) % 16],
        EDGES[iteration % 16],
        EDGES[(iteration * 7 + 1) % 16],
        EDGES[(iteration + 5) % 16],
    ]
    state[31] = 0x18000
    fp = family.startswith(('FP', 'FC', 'FMOV', 'SIMD_'))
    for lane in range(64):
        if iteration % 48 < 16:
            state[32 + lane] = EDGES[(iteration + lane) % 16]
        elif iteration % 48 < 32:
            state[32 + lane] = F32[(iteration + lane) % 16] | (F32[(iteration + lane + 3) % 16] << 32)
        else:
            state[32 + lane] = F64[(iteration + lane) % 16]
    if family in MEMORY:
        state[1], state[2] = 0x18000, 16
    if family in {'BR', 'BLR', 'RET'}:
        state[(word >> 5) & 31] = 0x1100
    state[96] = iteration & 15
    # Repeat edge patterns under RMode/FZ/DN/AHP for --states 1536.
    control = (iteration // 48) & 31 if fp else 0
    state[97] = ((control & 3) << 22) | ((control >> 2) << 24)
    state[98] = 0
    state[99] = PC
    return state


def build(emitter, directory, cc, cases, sanitize, compiler_flags):
    source = directory / 'generated.c'
    with source.open('w') as out:
        subprocess.run([str(emitter), *[f'{w:x}' for _, w in cases]], stdout=out, check=True)
    flags = ['-fsanitize=undefined', '-fno-sanitize=alignment', '-fsanitize-trap=undefined'] if sanitize else []
    library = directory / 'audit.so'
    subprocess.run(
        [
            cc,
            '-std=gnu11',
            '-O2',
            '-shared',
            '-fPIC',
            *compiler_flags,
            *flags,
            '-I',
            str(A64),
            '-I',
            str(ROOT / 'tests'),
            str(source),
            '-lm',
            '-latomic',
            '-o',
            str(library),
        ],
        check=True,
    )
    lib = ct.CDLL(str(library))
    lib.run.argtypes = [ct.c_uint, ct.c_uint, ct.c_uint, ct.POINTER(ct.c_uint64), ct.POINTER(ct.c_uint64), ct.c_void_p]
    lib.run.restype = ct.c_int
    return lib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    parser.add_argument('--match', default='')
    parser.add_argument('--states', type=int, default=48)
    parser.add_argument('--aliases', action='store_true')
    parser.add_argument('--word', type=lambda x: int(x, 16), help='one explicit word, matched to its decoder family')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--portable', action='store_true', help='omit the runtime -march=native/-mtune=native flags')
    parser.add_argument('--strict-c', action='store_true', help='omit -fwrapv to expose signed C overflow')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.states < 1:
        parser.error('--states must be positive')
    compiler_flags = ['-fno-strict-aliasing']
    if not args.portable:
        compiler_flags += ['-march=native', '-mtune=native']
    if not args.strict_c:
        compiler_flags += ['-fwrapv']
    arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    arm.ctl_set_cpu_model(reg.UC_CPU_ARM64_A57)
    arm.mem_map(0, MEM_SIZE)
    arm.reg_write(reg.UC_ARM64_REG_CPACR_EL1, 3 << 20)
    # EL0, as used by the recompiler. This does not model an A57's MMU/EL1+.
    arm.reg_write(reg.UC_ARM64_REG_PSTATE, 0)
    clean = arm.context_save()
    dis = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    report = {
        'model': 'Cortex-A57',
        'unicorn': '2.1.4',
        'states_per_allocated_word': args.states,
        'seed': '0xa57 + iteration',
        'compiler_flags': compiler_flags,
        'compiler': subprocess.check_output([args.cc, '--version'], text=True).splitlines()[0],
        'patterns': [],
        'cases': [],
        'totals': {},
    }
    # Snapshot includes the code page so PC-relative loads observe identical bytes.
    memory = bytes((i * 13 + 7) & 255 for i in range(MEM_SIZE))
    checks = mismatches = 0
    with tempfile.TemporaryDirectory(prefix='snx-a57-') as temp:
        directory = Path(temp)
        emitter = directory / 'emit'
        subprocess.run(
            [
                args.cc,
                '-O2',
                '-I',
                str(A64),
                '-I',
                str(ROOT / 'src/common/utilities'),
                str(ROOT / 'tests/a64_audit_emit.c'),
                str(A64 / 'a64.c'),
                '-o',
                str(emitter),
            ],
            check=True,
        )
        rows = inventory(emitter)
        report['patterns'] = [{'name': n, 'template_rows': count} for n, _, _, count, _ in rows]
        cases = [(f, w) for f, w in corpus(rows) if re.search(args.match, f)]
        if args.word is not None:
            family = next((f for f, mask, val, _, _ in rows if (args.word & mask) == val), 'unallocated')
            cases = [(family, args.word)]
        if args.aliases:
            seen = {w for _, w in cases}
            for family, word in list(cases):
                variants = []
                if family in {'FP2', 'FP3', 'DP2', 'DP3', 'SIMD_3same', 'SIMD_3diff', 'SIMD_elem'}:
                    variants += [(word & ~(31 << 16)) | (1 << 16)]  # identical source operands
                if family.startswith(('FP', 'FCVT', 'SIMD_')) and family not in MEMORY:
                    variants += [(word & ~31) | 1]  # destination aliases first source
                for variant in variants:
                    if variant not in seen:
                        cases.append((family, variant))
                        seen.add(variant)
        if not cases:
            parser.error('--match selected no cases')
        if any(f in SYSTEM for f, _ in cases):
            parser.error('runtime/system callbacks are reviewed separately, not executed by this bridge')
        lib = build(emitter, directory, args.cc, cases, args.sanitize, compiler_flags)
        for index, (family, word) in enumerate(cases):
            instruction = next(dis.disasm(word.to_bytes(4, 'little'), PC), None)
            label = instruction.mnemonic + ' ' + instruction.op_str if instruction else 'unallocated'
            entry = {
                'family': family,
                'word': f'{word:08x}',
                'instruction': label,
                'checks': 0,
                'mismatches': 0,
                'oracle': None,
            }
            report['cases'].append(entry)
            image = bytearray(memory)
            image[PC : PC + 4] = word.to_bytes(4, 'little')
            exclusive = family == 'LDSTX' and not (word & 0x00C00000) and (not (word & 0x00200000) or word >> 30 >= 2)
            if exclusive:
                image[PC - 8 : PC - 4] = (0xD5033F5F).to_bytes(4, 'little')  # CLREX
                prime = (word | (1 << 22) | (31 << 16)) & ~((31 << 10) | 31)
                prime |= ((15 if word & 0x00200000 else 31) << 10) | 14
                image[PC - 4 : PC] = prime.to_bytes(4, 'little')
            arm.mem_write(0, bytes(image))
            arm.ctl_remove_cache(PC - 8, PC + 4)
            for iteration in range(args.states):
                initial = state_for(word, family, iteration)
                arm.context_restore(clean)
                for r, v in zip(XREGS, initial[:32]):
                    arm.reg_write(r, v)
                for i, r in enumerate(VREGS):
                    arm.reg_write(r, initial[32 + 2 * i] | (initial[33 + 2 * i] << 64))
                arm.reg_write(reg.UC_ARM64_REG_NZCV, initial[96] << 28)
                arm.reg_write(reg.UC_ARM64_REG_FPCR, initial[97])
                arm.reg_write(reg.UC_ARM64_REG_FPSR, initial[98])
                arm.mem_write(0x10000, memory[0x10000:])
                error = None
                try:
                    if exclusive:
                        arm.emu_start(PC - 8, PC - 4, count=1)
                        if iteration & 1:
                            arm.emu_start(PC - 4, PC, count=1)
                            for r in (14, 15):
                                arm.reg_write(XREGS[r], initial[r])
                    arm.emu_start(PC, PC + 4, count=1)
                except UcError as exc:
                    error = str(exc)
                expected = [arm.reg_read(r) for r in XREGS]
                for r in VREGS:
                    v = arm.reg_read(r)
                    expected += [v & MASK, v >> 64]
                expected += [
                    arm.reg_read(reg.UC_ARM64_REG_NZCV) >> 28,
                    arm.reg_read(reg.UC_ARM64_REG_FPCR),
                    arm.reg_read(reg.UC_ARM64_REG_FPSR),
                    arm.reg_read(reg.UC_ARM64_REG_PC),
                ]
                expected_mem = bytes(arm.mem_read(0x10000, 0x10000)) if family in MEMORY else None
                entry['oracle'] = error or 'executed'
                for mode in (0, 1):
                    out = STATE()
                    native_mem = (ct.c_ubyte * MEM_SIZE).from_buffer_copy(image)
                    trap = lib.run(index, mode, word, STATE(*initial), out, native_mem)
                    delta = []
                    if error:
                        if not trap:
                            delta = ['accepted_oracle_rejection']
                    elif trap:
                        delta = ['unexpected_UNDEF']
                    else:
                        delta = [i for i in range(100) if out[i] != expected[i]]
                        if expected_mem is not None and bytes(native_mem)[0x10000:] != expected_mem:
                            delta += ['memory']
                    checks += 1
                    entry['checks'] += 1
                    if delta:
                        mismatches += 1
                        entry['mismatches'] += 1
                        if 'first_failure' not in entry:
                            entry['first_failure'] = {
                                'iteration': iteration,
                                'mode': 'interpreter' if mode else 'AOT',
                                'delta': delta,
                                'trap': trap,
                                'input': [f'{v:016x}' for v in initial],
                                'actual': [f'{v:016x}' for v in out],
                                'expected': [f'{v:016x}' for v in expected],
                            }
                if error:
                    break  # Allocation doesn't depend on data. Faults remain explicitly labelled.
            if index % 500 == 0:
                print(f'{index}/{len(cases)} words: {checks} comparisons, {mismatches} differences', flush=True)
    report['totals'] = {
        'words': len(cases),
        'comparisons': checks,
        'mismatches': mismatches,
        'failing_words': sum(bool(e['mismatches']) for e in report['cases']),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['totals']))
    return bool(mismatches)


if __name__ == '__main__':
    raise SystemExit(main())
