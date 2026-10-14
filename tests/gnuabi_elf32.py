#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Run minimal ELF32/AArch32 binaries through the GNU ABI runtime.

Crafts tiny static ELF32 executables (A32 and Thumb state) that sum 0..9 and
exit with the result, then checks the runner reports it. Machine code is
embedded so the test needs no ARM assembler at run time.

    gnuabi_elf32.py RUNNER
"""

import argparse
import pathlib
import struct
import subprocess
import tempfile

BASE = 0x10000

# sum 0..9 = 45, then exit(45). A32 words and Thumb halfwords, little-endian.
A32 = [0xE3A00000, 0xE3A01000, 0xE3A0200A, 0xE0800001, 0xE2811001, 0xE1510002, 0xBAFFFFFB, 0xE3A07001, 0xEF000000]
THUMB = [0xF04F, 0x0000, 0xF04F, 0x0100, 0xF04F, 0x020A, 0x4408, 0xF101, 0x0101, 0x4291, 0xDBFA, 0xF04F, 0x0701, 0xDF00]


def elf32(code: bytes, thumb: bool) -> bytes:
    entry_off = 0x80
    entry = BASE + entry_off + (1 if thumb else 0)
    filesz = entry_off + len(code)
    ehdr = bytearray(52)
    ehdr[0:4] = b'\x7fELF'
    ehdr[4] = 1  # ELFCLASS32
    ehdr[5] = 1  # little-endian
    ehdr[6] = 1
    struct.pack_into('<HHI', ehdr, 16, 2, 40, 1)  # ET_EXEC, EM_ARM, version
    struct.pack_into('<III', ehdr, 24, entry, 52, 0)  # entry, phoff, shoff
    struct.pack_into('<IHHHHHH', ehdr, 36, 0, 52, 32, 1, 0, 0, 0)
    phdr = bytearray(32)
    struct.pack_into('<IIIIIIII', phdr, 0, 1, 0, BASE, BASE, filesz, filesz, 5, 0x1000)
    image = bytearray(filesz)
    image[0:52] = ehdr
    image[52:84] = phdr
    image[entry_off : entry_off + len(code)] = code
    return bytes(image)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner')
    args = parser.parse_args()
    programs = {
        'a32-loop': b''.join(w.to_bytes(4, 'little') for w in A32),
        'thumb-loop': b''.join(h.to_bytes(2, 'little') for h in THUMB),
    }
    failures = 0
    with tempfile.TemporaryDirectory(prefix='gnuabi-elf32-') as directory:
        for name, code in programs.items():
            thumb = name.startswith('thumb')
            path = pathlib.Path(directory) / f'{name}.elf'
            path.write_bytes(elf32(code, thumb))
            result = subprocess.run([args.runner, str(path)], capture_output=True)
            if result.returncode != 45:
                print(f'FAIL {name}: exit {result.returncode}, expected 45')
                print(result.stderr.decode(errors='replace'))
                failures += 1
            else:
                print(f'PASS {name}: exit 45')
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
