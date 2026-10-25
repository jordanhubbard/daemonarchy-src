#!/usr/bin/env python3
"""Exercise the production diagnostic console with real UDP and subprocess IO."""
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

SCRIPT = pathlib.Path(__file__).with_name('debugnet-console.py')
HDR = struct.Struct('!IIQII')
ACK = struct.Struct('!I')


class ConsoleTest(unittest.TestCase):
    def test_bidirectional_mountroot_wire_and_capture(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            capture = pathlib.Path(tmp) / 'capture.bin'
            proc = subprocess.Popen([sys.executable, str(SCRIPT), '--bind', '127.0.0.1',
                '--boot-id', '0123456789abcdef', '--timeout', '5', '--capture', str(capture)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                sock.bind(('127.0.0.1', 0))
                sock.settimeout(2)
                dest = ('127.0.0.1', 26003)
                time.sleep(.1)
                sock.sendto(HDR.pack(1, 1, 0, 16, 0) + b'wrong-boot-id!!', dest)
                with self.assertRaises(socket.timeout):
                    sock.settimeout(.15)
                    sock.recvfrom(1024)
                sock.settimeout(2)
                sock.sendto(HDR.pack(1, 1, 0, 16, 0) + b'0123456789abcdef', dest)
                self.assertEqual(sock.recvfrom(1024)[0], ACK.pack(1))
                proc.stdin.write(b'ufs:/dev/ada0p2\n')
                proc.stdin.flush()
                wire, _ = sock.recvfrom(1024)
                self.assertEqual(HDR.unpack_from(wire)[:2], (3, 1))
                self.assertEqual(wire[HDR.size:], b'ufs:/dev/ada0p2\n')
                sock.sendto(ACK.pack(1), dest)
                output = b'\nmountroot> '
                sock.sendto(HDR.pack(3, 2, 0, len(output), 0) + output, dest)
                self.assertEqual(sock.recvfrom(1024)[0], ACK.pack(2))
                sock.sendto(HDR.pack(2, 3, 0, 0, 0), dest)
                self.assertEqual(sock.recvfrom(1024)[0], ACK.pack(3))
                proc.stdin.close()
                proc.wait(timeout=2)
                self.assertEqual(proc.returncode, 0, proc.stderr.read())
                self.assertEqual(capture.read_bytes(), output)
                self.assertEqual(proc.stdout.read(), output)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                if proc.stdin is not None and not proc.stdin.closed:
                    proc.stdin.close()
                proc.stdout.close()
                proc.stderr.close()


if __name__ == '__main__':
    unittest.main()
