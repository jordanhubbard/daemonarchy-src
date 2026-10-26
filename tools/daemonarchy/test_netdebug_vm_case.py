#!/usr/bin/env python3
"""Check the hard VM bound with a fake bhyve that ignores SIGTERM like a guest at db>."""
import importlib.util
import os
import pathlib
import shutil
import struct
import tempfile
import time
import unittest

SCRIPT = pathlib.Path(__file__).with_name("netdebug-vm-case.py")
SPEC = importlib.util.spec_from_file_location("netdebug_vm_case", SCRIPT)
CASE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CASE)

FAKE_SUDO = '#!/bin/sh\n[ "$1" = -n ] && shift\nexec "$@"\n'
# Like bhyve with a guest stopped in DDB: SIGTERM (the ACPI power button) is ignored.
FAKE_BHYVE = '''#!/usr/bin/env python3
import os, signal, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
open(os.environ["FAKE_VM_DIR"] + "/" + sys.argv[-1] + ".pid", "w").write(str(os.getpid()))
time.sleep(600)
'''
FAKE_BHYVECTL = '''#!/bin/sh
vm=${2#--vm=}
echo "$@" >> "$FAKE_VM_DIR/bhyvectl.log"
[ -f "$FAKE_VM_DIR/$vm.pid" ] && kill -9 "$(cat "$FAKE_VM_DIR/$vm.pid")"
exit 0
'''
# A timeout that never enforces its limit: only the destroy watchdog can end the guest.
FAKE_TIMEOUT = '#!/bin/sh\nshift 3\nexec "$@"\n'


class BoundTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        for name, body in (("sudo", FAKE_SUDO), ("bhyve", FAKE_BHYVE),
                           ("bhyvectl", FAKE_BHYVECTL)):
            self.tool(name, body)
        self.saved = {k: os.environ.get(k) for k in ("PATH", "FAKE_VM_DIR")}
        os.environ["PATH"] = f"{self.bin}{os.pathsep}{os.environ['PATH']}"
        os.environ["FAKE_VM_DIR"] = str(self.dir)

    def tearDown(self):
        for key, value in self.saved.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        self.tmp.cleanup()

    def tool(self, name, body):
        path = self.bin / name
        path.write_text(body)
        path.chmod(0o755)

    @unittest.skipUnless(shutil.which("timeout"), "timeout(1) is required")
    def test_sigterm_ignoring_guest_is_killed_at_hard_deadline(self):
        start = time.monotonic()
        status, destroyed = CASE.run_bounded_vm("vm-a", ["-c", "1"], 1, grace=1,
                                                destroy_after=30)
        elapsed = time.monotonic() - start
        # GNU timeout re-raises KILL on itself; BSD timeout exits 128 + 9.
        self.assertIn(status, (137, -9))
        self.assertFalse(destroyed)
        self.assertLess(elapsed, 10)

    def test_destroy_watchdog_ends_guest_when_timeout_does_not(self):
        self.tool("timeout", FAKE_TIMEOUT)
        start = time.monotonic()
        status, destroyed = CASE.run_bounded_vm("vm-b", ["-c", "1"], 1, grace=1,
                                                destroy_after=1)
        elapsed = time.monotonic() - start
        self.assertTrue(destroyed)
        self.assertNotEqual(status, 0)
        self.assertLess(elapsed, 10)
        self.assertIn("--destroy --vm=vm-b", (self.dir / "bhyvectl.log").read_text())

    def test_pcap_ack_extraction(self):
        def frame(sport, dport, payload):
            udp = struct.pack("!HHHH", sport, dport, 8 + len(payload), 0) + payload
            ip = bytes([0x45, 0]) + struct.pack("!H", 20 + len(udp)) + bytes(5) + b"\x11" \
                + bytes(10)
            return bytes(12) + b"\x08\x00" + ip + udp
        frames = [frame(26004, 26003, struct.pack("!I", 902)),
                  frame(26003, 26004, b"input"),
                  frame(26004, 26003, struct.pack("!I", 904))]
        pcap = self.dir / "t.pcap"
        body = struct.pack("<IHHiIII", 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1)
        for f in frames:
            body += struct.pack("<IIII", 0, 0, len(f), len(f)) + f
        pcap.write_bytes(body)
        payloads = list(CASE.udp_payloads(pcap, 26004, 26003))
        self.assertEqual(payloads, [struct.pack("!I", 902), struct.pack("!I", 904)])


if __name__ == "__main__":
    unittest.main()
