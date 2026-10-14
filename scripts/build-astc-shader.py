#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Assemble the checked-in ASTC SPIR-V source and embed it in a C header."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--spv', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / 'src/nx/gpu/texture/astc.spvasm'
    header = source.with_name('astc_spv.h')
    with tempfile.TemporaryDirectory() as temporary:
        binary = Path(temporary) / 'astc.spv'
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', str(source), '-o', str(binary)], check=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(binary)], check=True)
        data = binary.read_bytes()
    words = struct.unpack(f'<{len(data) // 4}I', data)
    text = """/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Generated from astc.spvasm by scripts/build-astc-shader.py.
   Derived from SwitchRecomp by MZ Labs, Copyright 2026, Apache-2.0.
   See LICENSE.apache and NOTICE. Do not edit. */
// clang-format off
static const u32 astc_spv[] = {
"""
    for i in range(0, len(words), 8):
        text += '    ' + ', '.join(f'0x{value:08x}' for value in words[i : i + 8]) + ',\n'
    text += '};\n// clang-format on\n'
    if args.check:
        if header.read_text() != text:
            raise SystemExit('astc_spv.h differs from astc.spvasm')
    else:
        header.write_text(text)
    if args.spv:
        args.spv.write_bytes(data)


if __name__ == '__main__':
    main()
