#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Rebuild the embedded VIC SPIR-V (the aligned and the unaligned build of vic.comp).
Requires glslangValidator on PATH (spirv-val is used when it is there).

The checked-in header is the compiled shader, so normal and cross builds do not
need a host shader compiler. --check verifies that it matches the GLSL source.
--spv PATH also saves the standalone binary of the aligned build.
"""

import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--spv', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / 'src/nx/gpu/video/vic.comp'
    header = source.with_name('vic_spv.h')
    version = (
        subprocess.check_output(['glslangValidator', '--version'], text=True).splitlines()[0].rsplit(':', 1)[-1].strip()
    )
    builds = [('', 'vic_spv'), ('-DVIC_UNALIGNED', 'vic_unaligned_spv')]
    words_by_name = {}
    with tempfile.TemporaryDirectory() as tmp:
        for define, name in builds:
            binary = Path(tmp) / f'{name}.spv'
            cmd = ['glslangValidator', '-V', '--target-env', 'vulkan1.1', '-o', str(binary), source.name]
            if define:
                cmd.insert(1, define)
            subprocess.run(cmd, cwd=source.parent, check=True)
            if shutil.which('spirv-val'):
                subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(binary)], check=True)
            data = binary.read_bytes()
            words_by_name[name] = struct.unpack(f'<{len(data) // 4}I', data)
            if args.spv and not define:
                args.spv.write_bytes(data)
    text = """/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Generated from vic.comp by scripts/build-vic-shader.py (glslang @VERSION@).
   Derived from SwitchRecomp by MZ Labs, Copyright 2026, Apache-2.0.
   See LICENSE.apache and NOTICE. Do not edit. */
// clang-format off
"""
    text = text.replace('@VERSION@', version)
    for _, name in builds:
        words = words_by_name[name]
        text += f'static const u32 {name}[] = {{\n'
        for i in range(0, len(words), 8):
            text += '    ' + ', '.join(f'0x{v:08x}' for v in words[i : i + 8]) + ',\n'
        text += '};\n'
    text += '// clang-format on\n'
    if args.check:
        if header.read_text() != text:
            raise SystemExit('vic_spv.h differs: regenerate with the glslang the header names, or review the compiler upgrade')
    else:
        header.write_text(text)


if __name__ == '__main__':
    main()
