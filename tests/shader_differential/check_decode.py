#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: Copyright 2026 ScarletNX contributors
"""Check patched decoder families over every upper-16-bit prefix."""

import argparse
import collections
import json
import re
import subprocess
from pathlib import Path


def run(args):
    return subprocess.run([str(x) for x in args], capture_output=True, text=True, check=True).stdout.splitlines()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--eden', type=Path, required=True)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--scarlet', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    assert run(['git', '-C', args.eden, 'rev-parse', 'HEAD']) == ['6352612425ae0765802eaa8c5904ff652b97222a']
    run(['git', '-C', args.eden, 'diff', '--exit-code', 'HEAD', '--', 'src/shader_recompiler'])
    source = (args.eden / 'src/shader_recompiler/frontend/maxwell/maxwell.inc').read_text()
    entries = []
    for name, cute, pattern in re.findall(r'INST\((\w+),\s*"([^"]+)",\s*"([01 -]+)"\)', source):
        pattern = pattern.replace(' ', '')
        entries.append(
            (name, cute, int(pattern.replace('-', '0'), 2), int(''.join('0' if c == '-' else '1' for c in pattern), 2))
        )
    actual = run([args.scarlet, '--decode-all'])
    assert len(actual) == 65536
    patched = {
        'CCTLL': 'CCTLL',
        'ATOMS_cas': 'ATOMS.CAS',
        'P2R_imm': 'P2R',
        'SUATOM': 'SUATOM',
        'SUATOM_cas': 'SUATOM.CAS',
        'CCTLT': 'CCTLT',
        'STP': 'STP',
    }
    selected, failures = collections.Counter(), collections.Counter()
    valid, reference_names = [], []
    for prefix, got in enumerate(actual):
        found = next((e for e in entries if prefix & e[3] == e[2]), None)
        if found:
            valid.append(prefix << 48)
            reference_names.append(found[1])
            if found[0] in patched:
                selected[found[0]] += 1
                if got != patched[found[0]]:
                    failures[found[0]] += 1
        # These Scarlet names must not claim encodings outside the reference family.
        if got in patched.values():
            allowed = {name for name, scarlet in patched.items() if scarlet == got}
            if got == 'P2R':
                allowed.update(('P2R_reg', 'P2R_cbuf'))
            if not found or found[0] not in allowed:
                failures['extra:' + got] += 1
    # Execute Eden's real decoder too. Unknown prefixes are deliberately not
    # passed to its asserting fallback, whose behavior is not an opcode oracle.
    decoded = []
    for start in range(0, len(valid), 500):
        decoded.extend(run([args.reference, '--decode', *[f'{w:016x}' for w in valid[start : start + 500]]]))
    assert decoded == reference_names, 'native reference decoder differs from table order'
    report = {
        'prefixes': 65536,
        'native_reference_prefixes': len(valid),
        'selected': selected,
        'failures': failures,
        'scope': 'Six corrected decoder entries and the STP guard, not all decoder discrepancies',
    }
    args.out.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
