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


class ProxyFaultTests(unittest.TestCase):
    """D5/D7 proxy faults over real sockets against the production program."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.kernel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.kernel.bind(("127.0.0.1", 0))
        self.kernel.settimeout(2)
        self.dest = ("127.0.0.1", PORT)
        self.proc = subprocess.Popen([sys.executable, str(SCRIPT), "--bind", "127.0.0.1",
            "--peer", "127.0.0.1", "--timeout", "1", "--capture",
            str(self.dir / "capture.bin"), "--events", str(self.dir / "events.jsonl")],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        time.sleep(.15)

    def tearDown(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.proc.stdout.close()
        self.proc.stderr.close()
        self.kernel.close()
        self.tmp.cleanup()

    def send(self, kind, seq, payload=b"", aux2=0):
        self.kernel.sendto(HEADER.pack(kind, seq, 0, len(payload), aux2) + payload,
                           self.dest)

    def ack(self):
        return ACK.unpack(self.kernel.recvfrom(1024)[0])[0]

    def summary(self):
        self.proc.wait(timeout=5)
        lines = (self.dir / "events.jsonl").read_text().splitlines()
        return json.loads(lines[-1])

    def test_reuse_reherald_malformed_and_out_of_order(self):
        self.kernel.sendto(b"tiny", self.dest)
        self.send(1, 1, aux2=VERSION)
        self.assertEqual(self.ack(), 1)
        self.kernel.sendto(HEADER.pack(3, 2, 0, 9, 0) + b"x", self.dest)
        self.send(7, 2, b"kind")
        self.send(3, 4, b"$late#00")  # ahead of sequence 2: must not be ACKed
        self.send(3, 2, b"$OK#9a")
        self.assertEqual(self.ack(), 2)
        self.send(3, 2, b"$OK#9a")  # lost ACK retransmission
        self.assertEqual(self.ack(), 2)
        self.send(3, 2, b"$E01#a6")  # new bytes after a timed-out kernel send
        self.assertEqual(self.ack(), 2)
        self.send(1, 1, aux2=VERSION)  # a new netgdb session after DDB re-entry
        self.assertEqual(self.ack(), 1)
        self.send(3, 2, b"$S05#b8")
        self.assertEqual(self.ack(), 2)
        self.send(2, 3)
        self.assertEqual(self.ack(), 3)
        summary = self.summary()
        self.assertEqual(self.proc.returncode, 0, self.proc.stderr.read())
        self.assertEqual((self.dir / "capture.bin").read_bytes(), b"$OK#9a$E01#a6$S05#b8")
        self.assertEqual(summary["outcome"], "kernel_finished")
        self.assertEqual((summary["malformed"], summary["unexpected"],
                          summary["out_of_order"], summary["duplicate_output"],
                          summary["sequence_reuse"], summary["reherald"]),
                         (2, 1, 1, 1, 1, 1))

    def test_unacknowledged_gdb_input_fails_with_record(self):
        self.send(1, 1, aux2=VERSION)
        self.assertEqual(self.ack(), 1)
        with socket.create_connection(("127.0.0.1", PORT + 1), 2) as gdb:
            gdb.sendall(b"$g#67")
            first = self.kernel.recvfrom(1024)[0]
            self.kernel.settimeout(1.5)
            retries = 0
            while True:
                try:
                    self.assertEqual(self.kernel.recvfrom(1024)[0], first)
                except socket.timeout:
                    break
                retries += 1
            summary = self.summary()
        self.assertEqual(self.proc.returncode, 1)
        self.assertEqual(retries, 5)
        self.assertEqual((summary["outcome"], summary["unconfirmed_input"],
                          summary["input_retransmit"]), ("error", 1, 5))

    def test_missing_kernel_idle_timeout_is_recorded(self):
        summary = self.summary()
        self.assertEqual(self.proc.returncode, 1)
        self.assertEqual((summary["outcome"], summary["admitted"]), ("idle_timeout", False))


if __name__ == "__main__":
    unittest.main()
