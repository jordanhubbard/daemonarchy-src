#!/usr/bin/env python3
"""Exercise the production NetGDB proxy over real TCP and UDP sockets."""

import json
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

SCRIPT = pathlib.Path(__file__).with_name("netgdb-proxy.py")
HEADER = struct.Struct("!IIQII")
ACK = struct.Struct("!I")
PORT = 20025
VERSION = 0x2515F095


class ProxyTests(unittest.TestCase):
    def test_native_wire_gdb_bridge_and_duplicate_output(self):
        with tempfile.TemporaryDirectory() as tmp, \
                socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as kernel:
            capture = pathlib.Path(tmp) / "capture.bin"
            events = pathlib.Path(tmp) / "events.jsonl"
            proc = subprocess.Popen([sys.executable, str(SCRIPT),
                "--bind", "127.0.0.1", "--peer", "127.0.0.1",
                "--timeout", "5", "--capture", str(capture),
                "--events", str(events)], stdout=subprocess.PIPE,
                stderr=subprocess.PIPE)
            try:
                kernel.bind(("127.0.0.1", 0))
                kernel.settimeout(2)
                time.sleep(.1)
                dest = ("127.0.0.1", PORT)
                kernel.sendto(HEADER.pack(1, 1, 0, 0, 0), dest)
                with self.assertRaises(socket.timeout):
                    kernel.settimeout(.15)
                    kernel.recvfrom(1024)
                kernel.settimeout(2)
                kernel.sendto(HEADER.pack(1, 1, 0, 0, VERSION), dest)
                self.assertEqual(kernel.recvfrom(1024)[0], ACK.pack(1))

                with socket.create_connection(("127.0.0.1", PORT + 1), 2) as gdb:
                    gdb.settimeout(2)
                    gdb.sendall(b"$?#3f")
                    wire, _ = kernel.recvfrom(1024)
                    self.assertEqual(HEADER.unpack_from(wire)[:2], (3, 1))
                    self.assertEqual(wire[HEADER.size:], b"$?#3f")
                    kernel.sendto(ACK.pack(1), dest)

                    response = HEADER.pack(3, 2, 0, 7, 7) + b"$S05#b8"
                    kernel.sendto(response, dest)
                    self.assertEqual(kernel.recvfrom(1024)[0], ACK.pack(2))
                    self.assertEqual(gdb.recv(1024), b"$S05#b8")
                    kernel.sendto(response, dest)
                    self.assertEqual(kernel.recvfrom(1024)[0], ACK.pack(2))
                    with self.assertRaises(socket.timeout):
                        gdb.settimeout(.15)
                        gdb.recv(1024)
                    kernel.sendto(HEADER.pack(2, 3, 0, 0, 0), dest)
                    self.assertEqual(kernel.recvfrom(1024)[0], ACK.pack(3))
                proc.wait(timeout=2)
                self.assertEqual(proc.returncode, 0, proc.stderr.read())
                self.assertEqual(capture.read_bytes(), b"$S05#b8")
                kinds = [json.loads(line)["event"] for line in events.read_text().splitlines()]
                self.assertIn("kernel_connected", kinds)
                self.assertIn("gdb_input_acked", kinds)
                self.assertIn("kernel_finished", kinds)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                proc.stdout.close()
                proc.stderr.close()


if __name__ == "__main__":
    unittest.main()
