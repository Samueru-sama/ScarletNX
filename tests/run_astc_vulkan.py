#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Build and run the headless ASTC Vulkan differential test on Linux.

Needs Clang/Clang++, Vulkan headers, volk sources, VMA headers, and a Vulkan 1.3
loader/driver. Software Vulkan (lavapipe) works. The three include/source paths
can point to the dependencies downloaded by the normal CMake build.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--vulkan-include', type=Path, required=True)
    parser.add_argument('--volk', type=Path, required=True, help='Directory containing volk.c and volk.h')
    parser.add_argument('--vma-include', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    flags = ['-O1', '-g', '-ffunction-sections', '-fdata-sections', '-DVK_NO_PROTOTYPES']
    for path in (repo / 'runtime', args.vulkan_include, args.volk, args.vma_include):
        flags += ['-I', str(path.resolve())]
    with tempfile.TemporaryDirectory(prefix='snx-astc-') as temp:
        out = Path(temp)
        sources = [
            repo / 'tests/astc_vulkan.c',
            repo / 'runtime/gpu/texture/astc.c',
            repo / 'runtime/gpu/vk/astc.c',
            repo / 'runtime/gpu/vk/cmd.c',
            args.volk.resolve() / 'volk.c',
            repo / 'runtime/gpu/vk/vma.cpp',
        ]
        objects = []
        for i, source in enumerate(sources):
            obj = out / f'{i}.o'
            cpp = source.suffix == '.cpp'
            subprocess.run(
                [
                    args.cxx if cpp else args.cc,
                    '-std=c++17' if cpp else '-std=gnu2x',
                    *flags,
                    '-c',
                    str(source),
                    '-o',
                    str(obj),
                ],
                check=True,
            )
            objects.append(str(obj))
        exe = out / 'astc_vulkan'
        subprocess.run([args.cxx, '-Wl,--gc-sections', *objects, '-ldl', '-pthread', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
