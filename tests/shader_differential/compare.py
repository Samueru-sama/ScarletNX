#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: Copyright 2026 ScarletNX contributors
"""Actual frontend emission + bounded CPU execution; not GLSL/SPIR-V/GPU testing."""

import argparse
import collections
import hashlib
import itertools
import json
import math
import random
import re
import struct
import subprocess
from pathlib import Path

MASK = 0xFFFFFFFF
POISON = object()
RECORD = struct.Struct('<262I')
EDEN_REV = '6352612425ae0765802eaa8c5904ff652b97222a'


def signed(x, bits=32):
    x &= (1 << bits) - 1
    return x - (1 << bits) if x >> (bits - 1) else x


def f32(x):
    return struct.unpack('<f', struct.pack('<I', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def execute(ir, original):
    """Interpret emitted IR. Unknown operations fail closed; no Maxwell equations."""
    state = list(original)
    values = []
    for inst in ir:
        op = inst['op']
        args = [
            values[a['ref']] if 'ref' in a else f32(a['value']) if a.get('type') == 2048 else a['value']
            for a in inst['args']
        ]
        a = args[0] if args else None
        b = args[1] if len(args) > 1 else None
        v = None
        if op == 'GetRegister':
            v = state[a] if a != 255 else 0
        elif op == 'SetRegister':
            if b is POISON:
                raise ValueError('observable undefined IR value')
            if a != 255:
                state[a] = int(b) & MASK
        elif op == 'GetPred':
            v = True if a == 7 else bool(state[256] & (1 << a))
        elif op == 'SetPred':
            if b is POISON:
                raise ValueError('observable undefined predicate')
            if a != 7:
                state[256] = (state[256] & ~(1 << a)) | (bool(b) << a)
        elif re.fullmatch('(Get|Set)[ZSCO]Flag', op):
            pos = 'ZSCO'.index(op[3])
            if op.startswith('Get'):
                v = bool(state[257] & (1 << pos))
            else:
                if a is POISON:
                    raise ValueError('observable undefined condition flag')
                state[257] = (state[257] & ~(1 << pos)) | (bool(a) << pos)
        elif op in ('GetCbufU32', 'GetCbufF32'):
            assert a == 0 and b in (0, 4, 8, 12), (a, b)
            v = state[258 + b // 4]
            if op == 'GetCbufF32':
                v = f32(v)
        elif op.startswith('Select'):
            v = POISON if a is POISON else b if a else args[2]
        elif any(x is POISON for x in args):
            v = POISON
        elif op == 'Identity':
            v = a
        elif op == 'LogicalNot':
            v = not a
        elif op == 'LogicalAnd':
            v = bool(a and b)
        elif op == 'LogicalOr':
            v = bool(a or b)
        elif op == 'LogicalXor':
            v = bool(a) != bool(b)
        elif op == 'GetZeroFromOp':
            v = a == 0
        elif op == 'GetSignFromOp':
            v = signed(a) < 0
        elif op.startswith('Bitwise'):
            if op == 'BitwiseNot32':
                v = ~a & MASK
            elif op == 'BitwiseAnd32':
                v = a & b
            elif op == 'BitwiseOr32':
                v = a | b
            elif op == 'BitwiseXor32':
                v = a ^ b
            else:
                raise ValueError(op)
        elif op in ('IAdd32', 'ISub32', 'INeg32', 'IAbs32'):
            if op == 'IAdd32':
                v = a + b
            elif op == 'ISub32':
                v = a - b
            elif op == 'INeg32':
                v = -a
            else:
                v = abs(signed(a))
            v &= MASK
        elif op.startswith(('UMin', 'UMax', 'SMin', 'SMax', 'UClamp', 'SClamp')):
            x = [signed(t) for t in args] if op.startswith('S') else args
            if 'Min' in op:
                v = min(x)
            elif 'Max' in op:
                v = max(x)
            else:
                v = min(max(x[0], x[1]), x[2])
            v &= MASK
        elif op.startswith(('IEqual', 'INotEqual', 'SLess', 'SGreater', 'ULess', 'UGreater')):
            x, y = (signed(a), signed(b)) if op[0] == 'S' else (a, b)
            relation = op[1:]
            v = {
                'Equal': x == y,
                'NotEqual': x != y,
                'LessThan': x < y,
                'LessThanEqual': x <= y,
                'GreaterThan': x > y,
                'GreaterThanEqual': x >= y,
            }[relation]
        elif op in ('BitFieldUExtract', 'BitFieldSExtract'):
            count = args[2]
            if b + count > 32:
                v = POISON
            elif count == 0:
                v = 0
            else:
                v = (a >> b) & ((1 << count) - 1)
                if op == 'BitFieldSExtract':
                    v = signed(v, count) & MASK
        elif op == 'BitFieldInsert':
            off, count = args[2:]
            if off + count > 32:
                v = POISON
            else:
                mask = ((1 << count) - 1) << off
                v = ((a & ~mask) | ((b << off) & mask)) & MASK
        elif op.startswith('Shift'):
            width = 64 if op.endswith('64') else 32
            assert b < width, (op, b)
            if 'Left' in op:
                v = a << b
            elif 'Arithmetic' in op:
                v = signed(a, width) >> b
            else:
                v = a >> b
            v &= (1 << width) - 1
        elif op.startswith('CompositeConstruct'):
            v = tuple(args)
        elif op.startswith('CompositeExtract'):
            v = a[b]
        elif op == 'PackUint2x32':
            v = a[0] | (a[1] << 32)
        elif op == 'UnpackUint2x32':
            v = (a & MASK, a >> 32)
        elif op == 'UnpackFloat2x16':
            v = struct.unpack('<ee', struct.pack('<I', a))
        elif op == 'BitCastF32U32':
            v = f32(a)
        elif op == 'BitCastU32F32':
            v = bits(a)
        elif op in ('FPAbs16', 'FPAbs32'):
            v = abs(a)
        elif op in ('FPNeg16', 'FPNeg32'):
            v = -a
        elif op in ('ConvertF32F16', 'FPConvertF32F16'):
            v = a
        elif op.startswith('FP') and re.search('(Ord|Unord|IsNan)', op):
            nan = any(math.isnan(x) for x in args)
            rel = re.sub(r'^FP(Unord|Ord)|\d+$', '', op)
            if 'IsNan' in op:
                v = nan
            elif op.startswith('FPOrdered'):
                v = not nan
            elif op.startswith('FPUnordered'):
                v = nan
            else:
                cmp = {
                    'Equal': a == b,
                    'NotEqual': a != b,
                    'LessThan': a < b,
                    'LessThanEqual': a <= b,
                    'GreaterThan': a > b,
                    'GreaterThanEqual': a >= b,
                }[rel]
                v = nan if nan else cmp
                if nan and not op.startswith('FPUnord'):
                    v = False
        elif op == 'FPRoundEven32':
            v = float(round(a))
        elif op == 'FPFloor32':
            v = float(math.floor(a))
        elif op == 'FPCeil32':
            v = float(math.ceil(a))
        elif op == 'FPTrunc32':
            v = float(math.trunc(a))
        elif op == 'FPClamp32':
            v = min(max(a, b), args[2])
        elif op in ('ConvertS32F32', 'ConvertU32F32'):
            v = int(a) & MASK
        else:
            raise ValueError(f'IR opcode not implemented in test executor: {op}')
        values.append(v)
    return tuple(state)


def cases(patterns):
    result = []
    rng = random.Random(0x534E58)
    edges = [0, 1, 0x7F, 0x80, 0xFF, 0xFFFF, 0x7FFFFFFF, 0x80000000, MASK]
    edges += [rng.getrandbits(32) for _ in range(3)]

    def word(name, fields):
        p = patterns[name]
        w = int(p.replace('-', '0'), 2) << 48
        for lo, width, val in [(16, 4, 7), *fields]:
            assert 0 <= val < (1 << width)
            w = (w & ~(((1 << width) - 1) << lo)) | (val << lo)
        assert all(ch == '-' or int(ch) == ((w >> (63 - i)) & 1) for i, ch in enumerate(p)), name
        return w

    def add(name, fields, inputs):
        w = word(name, fields)
        for regs, pred, flags, cb in inputs:
            result.append((name, w, regs, pred, flags, cb))

    def states(values=edges):
        return [({2: x, 3: 0x87654321}, i % 128, 15, x) for i, x in enumerate(values)]

    def operand(variant):
        return [(20, 19, 2 if variant == 'reg' else 0 if variant == 'cbuf' else 0x7FF80)]

    for variant, mask, dst in itertools.product(('reg', 'cbuf', 'imm'), (0, 1, 2, 15), (0, 255)):
        add('MOV_' + variant, operand(variant) + [(0, 8, dst), (39, 4, mask)], states())
    for mask in (0, 1, 2, 15):
        add('MOV32I', [(12, 4, mask), (20, 32, 0xA5A55A5A)], states())

    for variant, sf, df, ss, ds, sat, mods in itertools.product(
        ('reg', 'cbuf', 'imm'), range(3), range(3), range(2), range(2), range(2), range(4)
    ):
        selectors = (0, 1, 2, 3) if sf == 0 else (0, 2) if sf == 1 else (0,)
        # All selectors across the matrix, rather than a huge Cartesian product.
        sel = selectors[(df + ss + ds + mods) % len(selectors)]
        add(
            'I2I_' + variant,
            operand(variant)
            + [
                (8, 2, df),
                (10, 2, sf),
                (12, 1, ds),
                (13, 1, ss),
                (41, 3, sel),
                (49, 1, mods & 1),
                (45, 1, mods >> 1),
                (50, 1, sat),
                (47, 1, 1),
            ],
            states(),
        )
    add('I2I_reg', [(0, 8, 255), (20, 8, 2), (47, 1, 1)], states())

    for side, variant, size, wrap in itertools.product(('l', 'r'), ('reg', 'imm'), (0, 2, 3), range(2)):
        for shift in (0, 1, 2, 31, 32, 33, 63, 64, 255, 0x7FFFF):
            add(
                f'SHF_{side}_{variant}',
                [(8, 8, 3), (39, 8, 4), (37, 2, size), (50, 1, wrap), (20, 19, 2 if variant == 'reg' else shift)],
                [
                    ({2: shift, 3: lo, 4: hi}, 0x55, 15, 0)
                    for lo, hi in ((1, 0), (0x12345678, 0xF2345678), (MASK, MASK))
                ],
            )

    for variant, offset, count, dst in itertools.product(
        ('reg', 'rc', 'cr', 'imm'), (0, 8, 31, 32, 255), (0, 1, 33, 255), (0, 255)
    ):
        config = offset | (count << 8)
        fields = [(0, 8, dst), (8, 8, 3), (47, 1, 1)]
        if variant == 'reg':
            fields += [(20, 8, 2), (39, 8, 4)]
        elif variant == 'rc':
            fields += [(39, 8, 2)]
        elif variant == 'cr':
            fields += [(39, 8, 4)]
        else:
            fields += [(20, 19, config), (39, 8, 4)]
        add(
            'BFI_' + variant,
            fields,
            [
                ({2: config, 3: x, 4: 0xA5A5A5A5}, 0x55, 15, 0xA5A5A5A5 if variant == 'rc' else config)
                for x in (0, MASK, 0x12345678)
            ],
        )

    for variant, op, invert, po, dst in itertools.product(
        ('reg', 'cbuf', 'imm'), range(4), range(4), range(4), (0, 255)
    ):
        add(
            'LOP_' + variant,
            operand(variant)
            + [
                (0, 8, dst),
                (8, 8, 3),
                (41, 2, op),
                (39, 1, invert & 1),
                (40, 1, invert >> 1),
                (44, 2, po),
                (48, 3, 0),
                (47, 1, 1),
            ],
            states((0, MASK, 0x12345678)),
        )
    for op, dst in itertools.product(range(4), (0, 255)):
        add('LOP32I', [(0, 8, dst), (8, 8, 3), (20, 32, 0x87654321), (53, 2, op), (52, 1, 1)], states((0,)))

    for mask, offset, ccpr in itertools.product((0, 0x7F, 0xFF, 0x100, 0x7FFFF), range(4), range(2)):
        add(
            'P2R_imm',
            [(8, 8, 3), (20, 19, mask), (41, 2, offset), (40, 1, ccpr)],
            [({3: MASK}, p, f, 0) for p, f in ((0, 0), (0x55, 0xA), (0x7F, 0xF))],
        )

    pred_states = [({}, ((i & 1) << 2) | ((i & 2) << 2) | (i >> 2), 15, 0) for i in range(8)]
    aliases = ((0, 1), (1, 0), (0, 0), (7, 0), (0, 7), (2, 3))
    for bo1, bo2, neg, (pa, pb) in itertools.product(range(3), range(3), range(8), aliases):
        add(
            'PSETP',
            [
                (3, 3, pa),
                (0, 3, pb),
                (12, 3, 2),
                (29, 3, 3),
                (39, 3, 0),
                (24, 2, bo1),
                (45, 2, bo2),
                (15, 1, neg & 1),
                (32, 1, (neg >> 1) & 1),
                (42, 1, neg >> 2),
            ],
            pred_states,
        )
    for bf, bo1, bo2, dst in itertools.product(range(2), range(3), range(3), (0, 255)):
        add(
            'PSET',
            [(0, 8, dst), (12, 3, 2), (29, 3, 3), (39, 3, 0), (24, 2, bo1), (45, 2, bo2), (44, 1, bf), (47, 1, 1)],
            pred_states,
        )

    for name, bo, cond in itertools.product(('CSET', 'CSETP'), range(3), (0, 1, 2, 3, 7, 10, 15)):
        fields = [(8, 5, cond), (39, 3, 0), (45, 2, bo)]
        if name == 'CSETP':
            fields += [(0, 3, 1), (3, 3, 0)]
        else:
            fields += [(47, 1, 1)]
        add(name, fields, [({}, p, f, 0) for p in (0, 1) for f in range(16)])

    floats = [(0.0, 0.0), (1.0, 2.0), (3.0, -2.0), (float('nan'), 0.0)]
    for family, variant, bo, cmp in itertools.product(
        ('ISETP', 'FSETP', 'ISET', 'FSET'), ('reg', 'cbuf', 'imm'), range(3), (1, 2, 4, 5, 7)
    ):
        fields = operand(variant) + [(8, 8, 3), (39, 3, 0), (45, 2, bo)]
        fields += [(49, 3, cmp), (48, 1, 1)] if family.startswith('I') else [(48, 4, cmp)]
        if family.endswith('P'):
            fields += [(0, 3, 1), (3, 3, 0)]
        else:
            fields += [(0, 8, 255), (47, 1, 1)]
        vals = (
            [(bits(a), bits(b)) for a, b in floats] if family.startswith('F') else [(0, 0), (1, 2), (0x80000000, MASK)]
        )
        add(family + '_' + variant, fields, [({2: b, 3: a}, p, 15, b) for a, b in vals for p in (0, 1)])

    half_pairs = [(0, 0), (0x3C003C00, 0x3C004000), (0x40004000, 0x3C003C00)]
    for bo, hand, cmp, (pa, pb) in itertools.product(range(3), range(2), (1, 2, 4, 15), aliases):
        add(
            'HSETP2_reg',
            [(8, 8, 3), (20, 8, 2), (39, 3, 0), (45, 2, bo), (35, 4, cmp), (49, 1, hand), (3, 3, pa), (0, 3, pb)],
            [({2: b, 3: a}, p, 15, 0) for a, b in half_pairs for p in (0, 1)],
        )

    for variant, bo, hand, cmp in itertools.product(('cbuf', 'imm'), range(3), range(2), (1, 2, 4, 15)):
        add(
            'HSETP2_' + variant,
            [(8, 8, 3), (39, 3, 0), (45, 2, bo), (49, 4, cmp), (53, 1, hand), (3, 3, 0), (0, 3, 1)],
            [({3: a}, p, 15, bits(1.0)) for a in (0, 0x3C004000, 0x40004000) for p in (0, 1)],
        )

    for variant, mods, rounding in itertools.product(('reg', 'cbuf'), range(4), range(4)):
        add(
            'F2I_' + variant,
            operand(variant)
            + [(8, 2, 2), (10, 2, 2), (12, 1, 1), (45, 1, mods & 1), (49, 1, mods >> 1), (39, 2, rounding)],
            states([bits(x) for x in (0.0, -1.5, 2.5, -3.75, 123.25)]),
        )
    return result


def run(cmd, **kwargs):
    try:
        return subprocess.run([str(x) for x in cmd], check=True, capture_output=True, **kwargs).stdout
    except subprocess.CalledProcessError as e:
        print(e.stderr.decode() if isinstance(e.stderr, bytes) else e.stderr)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--eden', type=Path, required=True)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--scarlet', type=Path)
    p.add_argument('--cxx', required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--glslang', type=Path)
    p.add_argument('--spirv-val', type=Path)
    opts = p.parse_args()
    if bool(opts.glslang) != bool(opts.spirv_val):
        p.error('provide both --glslang and --spirv-val')
    assert run(['git', '-C', opts.eden, 'rev-parse', 'HEAD'], text=True).strip() == EDEN_REV
    run(
        [
            'git',
            '-C',
            opts.eden,
            'diff',
            '--exit-code',
            'HEAD',
            '--',
            'src/shader_recompiler',
            'src/common',
            'tools/maxwell-disas/file_environment.h',
        ]
    )
    opts.out.mkdir(parents=True, exist_ok=True)
    table = (opts.eden / 'src/shader_recompiler/frontend/maxwell/maxwell.inc').read_text()
    patterns = {n: pat.replace(' ', '') for n, pat in re.findall(r'INST\((\w+),\s*"[^"]+",\s*"([01 -]+)"\)', table)}
    vectors = cases(patterns)
    words = list(dict.fromkeys(v[1] for v in vectors))
    indices = {w: i for i, w in enumerate(words)}
    irs, snippets = {}, {}
    scarlet = opts.scarlet or opts.build / 'scarlet_frontend'
    for start in range(0, len(words), 100):
        batch = words[start : start + 100]
        ir_lines = run([opts.build / 'eden_frontend', *[f'{w:016x}' for w in batch]], text=True).splitlines()
        assert len(ir_lines) == len(batch)
        for w, line in zip(batch, ir_lines):
            irs[w] = json.loads(line)
        source = run([scarlet, *[f'{w:016x}' for w in batch]], text=True)
        parts = re.split(r'^// [^\n]+\n', source, flags=re.MULTILINE)[1:]
        assert len(parts) == len(batch)
        snippets.update(zip(batch, parts))
    (opts.out / 'reference-ir.json').write_text(json.dumps({f'{w:016x}': irs[w] for w in words}))
    cpp = ['#include "scalar.hpp"']
    for i, w in enumerate(words):
        # GLSL's unsuffixed floating literals are float, C++'s are double.
        src = re.sub(r'(?<![\w.])(\d+\.\d+(?:[eE][+-]?\d+)?)(?![\w.])', r'\1f', snippets[w])
        aliases = ''.join(f'auto& r{r}=s.r[{r}];' for r in sorted(set(map(int, re.findall(r'\br(\d+)\b', src)))))
        preds = ''.join(f'bool p{k}=(s.predicates & {1 << k}u)!=0;' for k in range(7))
        flags = ''.join(f'bool cc_{c}=(s.flags & {1 << k}u)!=0;' for k, c in enumerate('zsco'))
        pred_out = '|'.join(f'(uint(p{k})<<{k})' for k in range(7))
        flag_out = '|'.join(f'(uint(cc_{c})<<{k})' for k, c in enumerate('zsco'))
        cpp.append(
            f'void f{i}(State& s){{{aliases}{preds}{flags}CBuffer cb0(s.cbuf);\n{src}\ns.predicates={pred_out};s.flags={flag_out};}}'
        )
    cpp.append('using Fn=void(*)(State&); Fn functions[]={' + ','.join(f'f{i}' for i in range(len(words))) + '};')
    cpp.append(
        'int main(){std::fesetround(FE_TONEAREST);uint id;State s;while(fread(&id,4,1,stdin)==1){if(fread(&s,sizeof(s),1,stdin)!=1)return 2;functions[id](s);if(fwrite(&s,sizeof(s),1,stdout)!=1)return 3;}return ferror(stdin)?4:0;}'
    )
    source_path = opts.out / 'emitted.cpp'
    source_path.write_text('\n'.join(cpp))
    exe = (opts.out / 'emitted').resolve()
    run(
        [
            opts.cxx,
            '-std=c++23',
            '-O0',
            '-fwrapv',
            '-fsanitize=undefined',
            '-fno-sanitize-recover=all',
            '-I',
            Path(__file__).resolve().parent,
            source_path,
            '-o',
            exe,
        ]
    )
    stdin = bytearray()
    expected = []
    coverage = collections.Counter()
    for name, w, regs, pred, flags, cb in vectors:
        state = [0x12345678] * 255 + [0, pred, flags, cb, 0, 0, 0]
        for reg, val in regs.items():
            state[reg] = val
        stdin.extend(struct.pack('<I', indices[w]) + RECORD.pack(*state))
        try:
            expected.append(RECORD.pack(*execute(irs[w], state)))
        except Exception as e:
            raise RuntimeError(f'{name} {w:016x}: {e}') from e
        coverage[name] += 1
    output = run([exe], input=stdin)
    assert len(output) == len(vectors) * RECORD.size
    mismatches = collections.Counter()
    examples = []
    for i, (vector, want) in enumerate(zip(vectors, expected)):
        got = output[i * RECORD.size : (i + 1) * RECORD.size]
        if got != want:
            name, w, regs, pred, flags, cb = vector
            mismatches[name] += 1
            if len(examples) < 30:
                changes = [(j, x, y) for j, (x, y) in enumerate(zip(RECORD.unpack(got), RECORD.unpack(want))) if x != y]
                examples.append(
                    {
                        'name': name,
                        'word': f'{w:016x}',
                        'registers': regs,
                        'predicates': pred,
                        'flags': flags,
                        'cbuf': cb,
                        'differences': changes,
                    }
                )
    glsl_validated = False
    if opts.glslang:
        program = [0] * 10
        for w in [*words, 0xE30000000007000F]:
            if (len(program) - 10) % 4 == 0:
                program.append(0)
            program.append(w)
        binary = opts.out / 'synthetic.bin'
        binary.write_bytes(struct.pack('<' + 'Q' * len(program), *program))
        glsl = opts.out / 'synthetic.comp'
        glsl.write_bytes(run([scarlet, '--shader', binary]))
        spv = opts.out / 'synthetic.spv'
        run([opts.glslang, '-V', '--target-env', 'vulkan1.1', glsl, '-o', spv])
        run([opts.spirv_val, '--target-env', 'vulkan1.1', spv])
        print(run([scarlet, '--merge-test'], text=True).strip())
        glsl_validated = True
    report = {
        'glsl_spirv_validated': glsl_validated,
        'eden_revision': EDEN_REV,
        'cases': len(vectors),
        'words': len(words),
        'coverage': coverage,
        'mismatches': mismatches,
        'mismatch_count': sum(mismatches.values()),
        'examples': examples,
        'emitted_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
        'scope': 'Unoptimized visitors/IR vs production Scarlet instruction emission, CPU scalar shim; no GPU execution. Optional full shader GLSL/SPIR-V validation is compilation, not semantic execution.',
    }
    (opts.out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ('examples', 'coverage')}, indent=2))
    return bool(mismatches)


if __name__ == '__main__':
    raise SystemExit(main())
