# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Native UNIX stub: connect, stop, continue, interrupt, detach and lifetime cleanup."""

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def wait_for(predicate):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError('timed out')


def packet(sock, text):
    payload = text.encode()
    sock.sendall(b'$' + payload + b'#%02x' % (sum(payload) % 256))


def reply(sock):
    while True:
        byte = sock.recv(1)
        assert byte, 'stub disconnected'
        if byte != b'$':
            continue
        payload = b''
        while True:
            byte = sock.recv(1)
            assert byte
            if byte == b'#':
                break
            payload += byte
        checksum = sock.recv(1) + sock.recv(1)
        assert int(checksum, 16) == sum(payload) % 256
        sock.sendall(b'+')
        return payload


def main():
    if not shutil.which('gdbserver'):
        print('SKIP: gdbserver not installed')
        return 77
    probe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        missing = subprocess.run(
            [probe, '-s', str(root / 'unused')],
            env={**os.environ, 'PATH': str(root)},
            check=False,
            capture_output=True,
            timeout=5,
        )
        assert missing.returncode == 2 and b'requires gdbserver' in missing.stderr
        with socket.socket() as occupied:
            occupied.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            occupied.bind(('127.0.0.1', 1234))
            occupied.listen(1)
            busy = subprocess.run([probe, '-S', str(root / 'unused')], capture_output=True, timeout=5, check=False)
            assert busy.returncode == 2 and b'cannot start GDB stub' in busy.stderr
        for mode in ('-s', '-S'):
            marker = root / 'ran'
            marker.unlink(missing_ok=True)
            log = root / 'log'
            with log.open('wb') as output:
                process = subprocess.Popen([probe, mode, str(marker)], stderr=output, start_new_session=True)
            sock = None
            try:
                wait_for(lambda log=log: '1234' in log.read_text())
                if mode == '-s':
                    assert not marker.exists(), 'stopped startup executed before connection'
                else:
                    wait_for(marker.exists)
                sock = socket.create_connection(('127.0.0.1', 1234), timeout=10)
                packet(sock, 'qSupported')
                assert b'PacketSize' in reply(sock)
                packet(sock, '?')
                assert reply(sock).startswith((b'T', b'S'))
                packet(sock, 'c')
                try:
                    wait_for(marker.exists)
                except AssertionError:
                    print('continue reply:', reply(sock), file=sys.stderr)
                    raise
                sock.sendall(b'\x03')
                assert reply(sock).startswith((b'T', b'S'))
                packet(sock, 'D')
                assert reply(sock) == b'OK'
                sock.close()
                sock = None
            except Exception:
                print(log.read_text(), file=sys.stderr)
                raise
            finally:
                if sock:
                    sock.close()
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
        # An unconnected listener must disappear when its parent exits too.
        with (root / 'log').open('wb') as output:
            process = subprocess.Popen([probe, '-s', str(root / 'unused')], stderr=output, start_new_session=True)
        try:
            wait_for(lambda: '1234' in (root / 'log').read_text())
            process.kill()  # kill only the game, not its helper
            process.wait(timeout=5)

            def released():
                with socket.socket() as sock:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    try:
                        sock.bind(('127.0.0.1', 1234))
                        return True
                    except OSError:
                        return False

            wait_for(released)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
    print('GDB stub integration passed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
