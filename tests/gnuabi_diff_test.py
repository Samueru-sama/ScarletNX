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
            self._check_mismatch_replay(Path(temporary))

    @unittest.skipUnless(sys.platform == 'win32', 'Windows short paths')
    def test_mismatch_replay_with_windows_short_paths(self):
        import ctypes
        from ctypes import wintypes

        with tempfile.TemporaryDirectory(prefix='ScarletNX differential replay ') as temporary:
            get_short_path = ctypes.WinDLL('kernel32', use_last_error=True).GetShortPathNameW
            get_short_path.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
            get_short_path.restype = wintypes.DWORD
            required = get_short_path(temporary, None, 0)
            if not required:
                self.skipTest('Windows short paths are unavailable')
            buffer = ctypes.create_unicode_buffer(required)
            length = get_short_path(temporary, buffer, required)
            self.assertGreater(length, 0)
            self.assertLess(length, required)
            root = Path(buffer.value)
            if root == root.resolve():
                self.skipTest('The temporary filesystem does not provide an 8.3 alias')
            self.assertTrue(root.samefile(temporary))
            self._check_mismatch_replay(root)

    def _check_mismatch_replay(self, root):
        root = root / 'paths with spaces'
        root.mkdir()
        elf = root / 'test.elf'
        elf.write_bytes(b'test program')
        left, right = root / 'left.py', root / 'right.py'
        for script, output in ((left, 'actual'), (right, 'reference')):
            script.write_text(
                'import sys\nfrom pathlib import Path\n'
                'if Path(sys.argv[1]).read_bytes() != b"test program": raise SystemExit(2)\n'
                f'sys.stdout.buffer.write(b"{output}\\n")\n',
                encoding='ascii',
            )
        # Commands and artifact arguments retain the caller's path spelling.
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
        saved_elf = saved / 'program.elf'
        self.assertEqual(saved_elf.read_bytes(), elf.read_bytes())
        self.assertEqual((saved / 'scarletnx.stdout.bin').read_bytes(), b'actual\n')
        self.assertEqual((saved / 'qemu.stdout.bin').read_bytes(), b'reference\n')
        metadata = json.loads((saved / 'result.json').read_text())
        self.assertEqual(metadata['reasons'], ['stdout'])
        elf.unlink()
        for runtime in ('scarletnx', 'qemu'):
            with self.subTest(runtime=runtime):
                replay_argv = metadata['results'][runtime]['replay_argv']
                # Windows 8.3 aliases and expanded names identify the same file.
                self.assertTrue(Path(replay_argv[-1]).samefile(saved_elf))
                replay = subprocess.run(replay_argv, capture_output=True, check=False, timeout=15)
                self.assertEqual(replay.returncode, 0, replay.stderr.decode())
                self.assertEqual(replay.stdout, (saved / f'{runtime}.stdout.bin').read_bytes())


if __name__ == '__main__':
    unittest.main()
