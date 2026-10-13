#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Rebuild the embedded ASTC SPIR-V. Requires glslangValidator and spirv-val on PATH.

The checked-in header is the compiled shader, so normal and cross builds do not
need a host shader compiler. --check verifies that it matches the GLSL source.
--spv PATH also saves the standalone binary.
"""

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
    source = root / 'src/nx/gpu/texture/astc.comp'
    header = source.with_name('astc_spv.h')
    version = (
        subprocess.check_output(['glslangValidator', '--version'], text=True).splitlines()[0].rsplit(':', 1)[-1].strip()
    )
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'astc.spv'
        subprocess.run(
            ['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', str(binary), source.name],
            cwd=source.parent,
            check=True,
        )
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(binary)], check=True)
        data = binary.read_bytes()
    words = struct.unpack(f'<{len(data) // 4}I', data)
    text = """/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Generated from astc.comp by scripts/build-astc-shader.py (glslang @VERSION@).
   Derived from SwitchRecomp by MZ Labs, Copyright 2026, Apache-2.0.
   See LICENSE.apache and NOTICE. Do not edit. */
// clang-format off
static const u32 astc_spv[] = {
"""
    text = text.replace('@VERSION@', version)
    for i in range(0, len(words), 8):
        text += '    ' + ', '.join(f'0x{v:08x}' for v in words[i : i + 8]) + ',\n'
    text += '};\n// clang-format on\n'
    if args.check:
        if header.read_text() != text:
            raise SystemExit('astc_spv.h differs: regenerate with glslang 12.0.0, or review the compiler upgrade')
    else:
        header.write_text(text)
    if args.spv:
        args.spv.write_bytes(data)


if __name__ == '__main__':
    main()
