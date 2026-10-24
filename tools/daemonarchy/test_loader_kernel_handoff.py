"""Real UDP controls for authenticated phase/boot identity, not kernel acceptance."""

import importlib.util
import io
from pathlib import Path
import socket
import threading
import unittest

spec = importlib.util.spec_from_file_location(
    "loader_console", Path(__file__).with_name("loader-console.py")
)
console = importlib.util.module_from_spec(spec)
spec.loader.exec_module(console)


class HandoffTests(unittest.TestCase):
    def setUp(self):
        self.key = bytes(range(32))
        self.boot = bytes(range(16))
        self.loader_session = b"L" * 16
        self.kernel_session = b"K" * 16
        self.server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.settimeout(1)
        self.events = io.BytesIO()
        self.client = console.Console(
            "127.0.0.1",
            self.server.getsockname()[1],
            self.key,
            bind="127.0.0.1",
            timeout=0.2,
            events=self.events,
        )
        self.client.session, self.client.boot = self.loader_session, self.boot
        self.client.handoff = True
        self.errors = []

    def tearDown(self):
        self.client.close()
        self.server.close()

    def run_peer(self, reply):
        def peer():
            try:
                data, address = self.server.recvfrom(2048)
                message = console.decode(self.key, data, console.ZERO)
                assert message is not None and message[0] == console.CHALLENGE
                reply(message[3], address)
            except BaseException as error:
                self.errors.append(error)

        thread = threading.Thread(target=peer)
        thread.start()
        return thread

    def welcome(self, challenge, *, kind=None, boot=None, session=None):
        return console.encode(
            self.key,
            self.kernel_session if session is None else session,
            console.KERNEL_WELCOME if kind is None else kind,
            0,
            challenge + (self.boot if boot is None else boot),
        )

    def test_only_fresh_kernel_role_for_same_boot_closes_handoff(self):
        def reply(challenge, address):
            rejected = [
                self.welcome(challenge, kind=console.WELCOME),
                self.welcome(challenge, boot=b"X" * 16),
                self.welcome(challenge, session=self.loader_session),
                self.welcome(b"old-challenge!!!".ljust(16, b"!")),
                self.welcome(challenge, session=console.ZERO),
            ]
            forged = bytearray(self.welcome(challenge))
            forged[-1] ^= 1
            for packet in [*rejected, bytes(forged), self.welcome(challenge)]:
                self.server.sendto(packet, address)

        thread = self.run_peer(reply)
        self.client.pair(kernel=True)
        thread.join(1)
        self.assertFalse(thread.is_alive())
        self.assertEqual(self.errors, [])
        self.assertEqual(self.client.phase, "kernel")
        self.assertEqual(self.client.boot, self.boot)
        self.assertEqual(self.client.session, self.kernel_session)
        self.assertFalse(self.client.handoff)
        self.assertIn(b'"event": "kernel_connected"', self.events.getvalue())
        self.assertIn(b'"event": "kernel_identity_refused"', self.events.getvalue())

    def test_loader_response_cannot_claim_kernel_connection(self):
        thread = self.run_peer(
            lambda challenge, address: self.server.sendto(
                self.welcome(challenge, kind=console.WELCOME), address
            )
        )
        with self.assertRaises(TimeoutError):
            self.client.pair(kernel=True)
        thread.join(1)
        self.assertEqual(self.errors, [])
        self.assertTrue(self.client.handoff)
        self.assertEqual(self.client.phase, "firmware")
        self.assertNotIn(b'"event": "kernel_connected"', self.events.getvalue())
        self.assertIn(
            b'"event": "kernel_connection_unobserved"', self.events.getvalue()
        )

    def test_missing_peer_never_becomes_kernel_evidence(self):
        with self.assertRaises(TimeoutError):
            self.client.pair(kernel=True)
        self.assertTrue(self.client.handoff)
        self.assertEqual(self.client.session, self.loader_session)
        self.assertNotIn(b'"kernel_observed": true', self.events.getvalue())

    def test_kernel_pairing_requires_a_prior_firmware_boot_and_handoff(self):
        self.client.handoff = False
        with self.assertRaises(ValueError):
            self.client.pair(kernel=True)
        self.client.handoff = True
        self.client.boot = None
        with self.assertRaises(ValueError):
            self.client.pair(kernel=True)


if __name__ == "__main__":
    unittest.main()
