"""Production CLI over loopback UDP; these peers are not native kernel evidence."""
import hmac
import json
from pathlib import Path
import socket
import signal
import struct
import subprocess
import sys
import tempfile
import unittest

HEADER = struct.Struct("!4sBBHQ16s")
KEY = bytes(range(32))
BOOT = b"B" * 16
LOADER = b"L" * 16
KERNEL = b"K" * 16
ZERO = bytes(16)
INPUT, OUTPUT, ACK, FINISH, CHALLENGE, WELCOME, OUTPUT_ACK, KERNEL_WELCOME = range(2, 10)


def packet(kind, sequence, session, payload=b"", key=KEY):
    data = HEADER.pack(b"LDNC", 1, kind, len(payload), sequence, session) + payload
    return data + hmac.digest(key, data, "sha256")


class CLITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.key = self.root / "key"
        self.key.write_bytes(KEY)
        self.key.chmod(0o600)
        self.server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.settimeout(2)
        self.process = None
        self.wire_key = KEY

    def tearDown(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.kill()
            self.process.communicate(timeout=3)
        self.server.close()
        self.temp.cleanup()

    def start(self, follow=True, plain=False):
        args = [sys.executable, str(Path(__file__).with_name("loader-console.py")),
                "127.0.0.1", "--port", str(self.server.getsockname()[1]),
                "--timeout", "0.6", "--command", "boot",
                "--output", str(self.root / "output"), "--events", str(self.root / "events")]
        self.wire_key = bytes(32) if plain else KEY
        args += ["--plain"] if plain else ["--key-file", str(self.key)]
        if follow:
            args.append("--follow-kernel")
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        kind, sequence, session, challenge = self.receive()
        self.assertEqual((kind, sequence, session), (CHALLENGE, 0, ZERO))
        self.send(WELCOME, 0, LOADER, challenge + BOOT)
        kind, sequence, session, payload = self.receive()
        self.assertEqual((kind, sequence, session, payload), (INPUT, 1, LOADER, b"boot\n"))
        self.send(OUTPUT, 1, LOADER, b"loader handoff\n")
        self.assertEqual(self.receive()[:3], (OUTPUT_ACK, 1, LOADER))
        self.send(FINISH, 2, LOADER)

    def receive(self):
        data, self.address = self.server.recvfrom(2048)
        magic, version, kind, size, sequence, session = HEADER.unpack_from(data)
        self.assertEqual((magic, version, len(data)), (b"LDNC", 1, 64 + size))
        self.assertEqual(data[-32:], hmac.digest(self.wire_key, data[:-32], "sha256"))
        return kind, sequence, session, data[32:-32]

    def send(self, kind, sequence, session, payload=b""):
        self.server.sendto(packet(kind, sequence, session, payload, self.wire_key), self.address)

    def finish(self):
        stdout, stderr = self.process.communicate(timeout=3)
        events = [json.loads(line) for line in (self.root / "events").read_text().splitlines()]
        return self.process.returncode, stdout, stderr, events

    def kernel_challenge(self):
        kind, sequence, session, challenge = self.receive()
        self.assertEqual((kind, sequence, session), (CHALLENGE, 0, ZERO))
        return challenge

    def test_follow_kernel_blank_mountroot_and_capture_after_stdin_eof(self):
        self.start()
        challenge = self.kernel_challenge()
        self.send(KERNEL_WELCOME, 0, KERNEL, challenge + BOOT)
        self.send(OUTPUT, 1, KERNEL, b"mountroot> ")
        self.assertEqual(self.receive()[:3], (OUTPUT_ACK, 1, KERNEL))
        self.process.stdin.write(b"\n")
        self.process.stdin.flush()
        self.assertEqual(self.receive(), (INPUT, 1, KERNEL, b"\n"))
        self.send(ACK, 1, KERNEL)
        self.process.stdin.write(b"ufs:/dev/test\n")
        self.process.stdin.flush()
        # No loader echo marker may be injected into the kernel prompt.
        self.assertEqual(self.receive(), (INPUT, 2, KERNEL, b"ufs:/dev/test\n"))
        self.send(ACK, 2, KERNEL)
        self.process.stdin.close()
        self.process.stdin = None
        self.send(OUTPUT, 2, KERNEL, b"panic: retained after EOF\n")
        self.assertEqual(self.receive()[:3], (OUTPUT_ACK, 2, KERNEL))
        self.send(FINISH, 3, KERNEL)
        code, stdout, stderr, events = self.finish()
        self.assertEqual((code, stderr), (0, b""))
        self.assertEqual((self.root / "output").read_bytes(), stdout)
        self.assertIn(b"panic: retained after EOF", stdout)
        self.assertEqual([e["event"] for e in events],
                         ["paired", "firmware_handoff_started", "kernel_connected", "kernel_transport_closed"])
        self.assertTrue(events[-1]["kernel_observed"])

    def test_wrong_boot_cannot_claim_kernel_and_preserves_loader_capture(self):
        self.start()
        challenge = self.kernel_challenge()
        self.send(KERNEL_WELCOME, 0, KERNEL, challenge + b"X" * 16)
        code, stdout, stderr, events = self.finish()
        self.assertEqual(code, 1)
        self.assertIn(b"no authenticated kernel response for this boot", stderr)
        self.assertEqual((self.root / "output").read_bytes(), b"loader handoff\n")
        self.assertNotIn("kernel_connected", [e["event"] for e in events])
        self.assertIn("kernel_identity_refused", [e["event"] for e in events])
        self.assertFalse(events[-1]["kernel_observed"])

    def test_missing_kernel_times_out_without_losing_capture(self):
        self.start()
        self.kernel_challenge()
        code, stdout, stderr, events = self.finish()
        self.assertEqual(code, 1)
        self.assertEqual((self.root / "output").read_bytes(), stdout)
        self.assertEqual(events[-1]["event"], "kernel_connection_unobserved")
        self.assertFalse(events[-1]["kernel_observed"])

    def test_default_still_exits_at_firmware_handoff(self):
        self.start(follow=False)
        code, stdout, stderr, events = self.finish()
        self.assertEqual((code, stderr), (3, b""))
        self.assertEqual(events[-1]["event"], "firmware_handoff_started")
        self.assertNotIn("kernel_connected", [e["event"] for e in events])

    def test_explicit_plain_mode_works_without_a_key_file(self):
        self.start(follow=False, plain=True)
        code, stdout, stderr, events = self.finish()
        self.assertEqual((code, stderr), (3, b""))
        self.assertIn(b"loader handoff", stdout)
        self.assertEqual(events[-1]["event"], "firmware_handoff_started")

    def test_keyboard_interrupt_closes_capture_without_traceback(self):
        self.start()
        challenge = self.kernel_challenge()
        self.send(KERNEL_WELCOME, 0, KERNEL, challenge + BOOT)
        self.send(OUTPUT, 1, KERNEL, b"last stage before operator stop\n")
        self.assertEqual(self.receive()[:3], (OUTPUT_ACK, 1, KERNEL))
        self.process.send_signal(signal.SIGINT)
        code, stdout, stderr, events = self.finish()
        self.assertEqual((code, stderr), (130, b""))
        self.assertEqual(events[-1]["event"], "controller_stopped")
        self.assertTrue(events[-1]["kernel_observed"])
        self.assertEqual((self.root / "output").read_bytes(), stdout)

    def test_kernel_idle_deadline_preserves_observed_phase(self):
        self.start()
        challenge = self.kernel_challenge()
        self.send(KERNEL_WELCOME, 0, KERNEL, challenge + BOOT)
        self.send(OUTPUT, 1, KERNEL, b"last kernel stage\n")
        self.assertEqual(self.receive()[:3], (OUTPUT_ACK, 1, KERNEL))
        code, stdout, stderr, events = self.finish()
        self.assertEqual(code, 1)
        self.assertIn(b"no reboot is inferred", stderr)
        self.assertEqual(events[-1]["event"], "capture_idle_timeout")
        self.assertTrue(events[-1]["kernel_observed"])
        self.assertEqual((self.root / "output").read_bytes(), stdout)


if __name__ == "__main__":
    unittest.main()
