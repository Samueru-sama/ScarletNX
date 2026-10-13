# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Test differential failure capture without requiring AArch64 build tools."""

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1] / 'tools/gnuabi_diff.py'
spec = importlib.util.spec_from_file_location('gnuabi_diff', TOOL)
diff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diff)


class DifferentialTests(unittest.TestCase):
    def test_seed_reproduces_source(self):
        self.assertEqual(diff.fuzz_source(42), diff.fuzz_source(42))
        self.assertNotEqual(diff.fuzz_source(42), diff.fuzz_source(43))

    def test_matching_failure_is_not_a_pass(self):
        result = {'returncode': -11, 'timeout': False, 'stdout': b'', 'stderr': b''}
        self.assertIn('host signal/crash', diff.mismatches(result, result, False))
        result = {**result, 'returncode': None, 'timeout': True}
        self.assertIn('timeout', diff.mismatches(result, result, False))

    def test_mismatch_saves_elf_and_both_streams(self):
        with tempfile.TemporaryDirectory(prefix='snx-diff-test-') as temporary:
            root = Path(temporary)
            elf = root / 'test.elf'
            elf.write_bytes(b'test program')
            left, right = root / 'left.py', root / 'right.py'
            left.write_text('import sys\nsys.stdout.buffer.write(b"actual\\n")\n', encoding='ascii')
            right.write_text('import sys\nsys.stdout.buffer.write(b"reference\\n")\n', encoding='ascii')
            # Quoting a command through the tool exercises paths with spaces.
            result = subprocess.run(
                [
                    sys.executable,
                    str(TOOL),
                    str(elf),
                    '--scarletnx',
                    f'"{sys.executable}" "{left}"',
                    '--qemu',
                    f'"{sys.executable}" "{right}"',
                    '--artifact-dir',
                    str(root / 'artifacts'),
                ],
                capture_output=True,
                check=False,
                timeout=15,
            )
            self.assertEqual(result.returncode, 1, result.stderr.decode())
            saved = next((root / 'artifacts').iterdir())
            self.assertEqual((saved / 'program.elf').read_bytes(), elf.read_bytes())
            self.assertEqual((saved / 'scarletnx.stdout.bin').read_bytes(), b'actual\n')
            self.assertEqual((saved / 'qemu.stdout.bin').read_bytes(), b'reference\n')
            metadata = json.loads((saved / 'result.json').read_text())
            self.assertEqual(metadata['reasons'], ['stdout'])
            self.assertIn(str(saved / 'program.elf'), metadata['results']['qemu']['replay_argv'])


if __name__ == '__main__':
    unittest.main()
