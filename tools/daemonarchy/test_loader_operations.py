#!/usr/bin/env python3
"""D1/D2 controller checks against a loopback loader emulator.

The emulator speaks the plain LDNC wire format and interprets the standard
loader commands the controller sends (set, unset, show, echo, unload, load,
include, netdebug stop), fetching net0: paths from the controller's TFTP
server the way libsa does.  It is controller regression evidence only; the
real loader and its Lua interpreter run in the native VM cases.
"""
import hmac
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from netdebug_stage import StageRecorder  # noqa: E402
import tftp_server  # noqa: E402

SCRIPT = Path(__file__).with_name("loader-console.py")
HEADER = struct.Struct("!4sBBHQ16s")
INPUT, OUTPUT, INPUT_ACK, FINISH, CHALLENGE, WELCOME, OUTPUT_ACK = range(2, 9)
KEY = bytes(32)
BOOT = bytes.fromhex("00112233445566778899aabbccddeeff")


def packet(kind, sequence, session, payload=b""):
    data = HEADER.pack(b"LDNC", 1, kind, len(payload), sequence, session) + payload
    return data + hmac.digest(KEY, data, "sha256")


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class LoaderEmulator(threading.Thread):
    def __init__(self, *, marker=True, silent_after=None, panic_after=None):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(.1)
        self.env = {"currdev": "disk0p2:", "existing": "old"}
        self.commands, self.loaded, self.included = [], {}, {}
        self.sessions = 0
        self.marker, self.silent_after, self.panic_after = marker, silent_after, panic_after
        self.stopped = threading.Event()
        self.done = threading.Event()
        self.peer = None
        self.session = None
        self.out_seq = 1
        self.in_seq = 0

    @property
    def port(self):
        return self.sock.getsockname()[1]

    def say(self, text):
        self.sock.sendto(packet(OUTPUT, self.out_seq, self.session, text.encode()), self.peer)
        self.out_seq += 1

    def run(self):
        while not self.done.is_set():
            try:
                data, peer = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            magic, version, kind, size, sequence, session = HEADER.unpack_from(data)
            if data[-32:] != hmac.digest(KEY, data[:-32], "sha256"):
                continue
            payload = data[32:-32]
            if self.stopped.is_set():
                continue  # revoked: the loader no longer answers
            if kind == CHALLENGE:
                self.sessions += 1
                self.peer = peer
                self.session = bytes([0x40 + self.sessions]) * 16
                self.out_seq, self.in_seq = 1, 0
                self.sock.sendto(packet(WELCOME, 0, self.session, payload + BOOT), peer)
                if self.marker:
                    self.say(f"netdebug-stage: efi-loader boot_id={BOOT.hex()} nic=net0 "
                             "pci=0:0:3:0 earlier=uninstrumented\n")
            elif kind == INPUT and session == self.session:
                self.sock.sendto(packet(INPUT_ACK, sequence, self.session), peer)
                if sequence <= self.in_seq:
                    continue
                self.in_seq = sequence
                self.execute(payload.decode().rstrip("\n"))

    def fetch(self, path):
        root = self.env["net.static.rootpath"]
        server, port = root[len("tftp://"):].rstrip("/").split(":")
        data, size = tftp_server.fetch(server, int(port), path[len("net0:"):])
        assert size == len(data)
        return data

    def execute(self, line):
        words = line.split()
        if not words:
            return
        self.commands.append(line)
        if self.silent_after is not None and len(self.commands) > self.silent_after:
            if self.panic_after:
                self.say("panic: emulated loader fault\n")
            self.stopped.set()  # hard reset: nothing more is sent
            return
        verb = words[0]
        if verb == "echo":
            self.say(" ".join(words[1:]) + "\n")
        elif verb == "set":
            name, _, value = words[1].partition("=")
            self.env[name] = value
        elif verb == "unset":
            self.env.pop(words[1], None)
        elif verb == "show":
            value = self.env.get(words[1])
            self.say(f"{value}\n" if value is not None
                     else f"show: variable '{words[1]}' not found\n")
        elif verb == "unload":
            self.loaded.clear()
        elif verb in ("load", "include"):
            path = words[1]
            data = self.fetch(path) if path.startswith("net0:") else path.encode()
            (self.loaded if verb == "load" else self.included)[path] = data
            self.say(f"{path} size=0x{len(data):x}\n")
        elif words[:2] == ["netdebug", "stop"]:
            self.say("Wired debug console revoked.\n")
            self.stopped.set()

    def close(self):
        self.done.set()
        self.join(2)
        self.sock.close()


class OperationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.root = self.dir / "tftp"
        self.root.mkdir()
        self.kernel = bytes(range(256)) * 37  # several 1428-byte blocks
        (self.root / "kernel").write_bytes(self.kernel)
        self.lua = b'loader.setenv("staged.lua", "ran")\nprint("staged Lua ran")\n'
        (self.root / "stage.lua").write_bytes(self.lua)
        self.emulators = []

    def tearDown(self):
        for emulator in self.emulators:
            emulator.close()
        self.tmp.cleanup()

    def emulator(self, **kw):
        emulator = LoaderEmulator(**kw)
        emulator.start()
        self.emulators.append(emulator)
        return emulator

    def run_controller(self, emulator, *extra, name="run", timeout="2"):
        args = [sys.executable, str(SCRIPT), "127.0.0.1", "--port", str(emulator.port),
                "--plain", "--bind", "127.0.0.1", "--timeout", timeout,
                "--output", str(self.dir / f"{name}.out"),
                "--events", str(self.dir / f"{name}.events"),
                "--last-stage", str(self.dir / f"{name}.stage"), *extra]
        result = subprocess.run(args, stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
        events = [json.loads(line) for line in
                  (self.dir / f"{name}.events").read_text().splitlines()]
        stage = json.loads((self.dir / f"{name}.stage").read_text())
        return result, events, stage

    def test_variables_kernel_module_tftp_lua_and_stop(self):
        emulator = self.emulator()
        port = free_udp_port()
        result, events, stage = self.run_controller(
            emulator, "--tftp-root", str(self.root), "--tftp-port", str(port),
            "--static-ip", "127.0.0.1", "--set", "kern.msgbufsize=131072",
            "--unset", "existing", "--show", "kern.msgbufsize", "--show", "existing",
            "--kernel", "net0:/kernel", "--module", "/boot/kernel/if_em.ko",
            "--lua", "net0:/stage.lua", "--stop")
        self.assertEqual(result.returncode, 0, result.stderr)
        loader_lines = [c for c in emulator.commands if not c.startswith("echo LDNC-")]
        self.assertEqual(loader_lines, [
            "set net.static.ip=127.0.0.1",
            f"set net.static.rootpath=tftp://127.0.0.1:{port}/",
            "set kern.msgbufsize=131072", "unset existing",
            "show kern.msgbufsize", "show existing", "unload", "load net0:/kernel",
            "load /boot/kernel/if_em.ko", "include net0:/stage.lua", "netdebug stop"])
        self.assertEqual(emulator.loaded["net0:/kernel"], self.kernel)
        self.assertEqual(emulator.included["net0:/stage.lua"], self.lua)
        shown = [(e["name"], e["output"]) for e in events if e["event"] == "variable_shown"]
        self.assertEqual(shown, [("kern.msgbufsize", "131072"),
                                 ("existing", "show: variable 'existing' not found")])
        tftp = [e for e in events if e["event"] == "tftp_complete"]
        self.assertEqual(sorted((e["file"], e["bytes"]) for e in tftp),
                         [("/kernel", len(self.kernel)), ("/stage.lua", len(self.lua))])
        # TFTP completion is reported from the server thread and may land later.
        self.assertEqual([e["event"] for e in events
                          if not e["event"].startswith("tftp_")][-1], "console_stopped")
        self.assertEqual((stage["earliest_stage"], stage["outcome"]), ("efi-loader", "stopped"))
        self.assertEqual(stage["markers"][0]["pci"], "0:0:3:0")
        self.assertEqual(stage["boot_id"], BOOT.hex())

    def test_values_that_the_loader_would_reinterpret_are_refused(self):
        emulator = self.emulator()
        for bad in (["--set", "a=b;boot"], ["--set", "a=$HOME"], ["--set", 'a="x"'],
                    ["--set", "a=two words"], ["--kernel", "/boot/kernel kernel"],
                    ["--unset", "a b"], ["--tftp-root", str(self.root)]):
            with self.subTest(bad=bad):
                name = f"bad{abs(hash(tuple(bad)))}"
                args = [sys.executable, str(SCRIPT), "127.0.0.1", "--port",
                        str(emulator.port), "--plain", "--timeout", "1",
                        "--events", str(self.dir / name), *bad]
                result = subprocess.run(args, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 1)
                self.assertFalse((self.dir / name).exists())
        self.assertEqual(emulator.sessions, 0)

    def test_controller_restart_repairs_a_new_session(self):
        emulator = self.emulator()
        first, _, _ = self.run_controller(emulator, "--set", "a=1", name="first")
        self.assertEqual(first.returncode, 0, first.stderr)
        second, events, stage = self.run_controller(emulator, "--show", "a", name="second")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(emulator.sessions, 2)
        self.assertIn(("a", "1"), [(e["name"], e["output"]) for e in events
                                   if e["event"] == "variable_shown"])
        self.assertEqual(stage["outcome"], "operator_closed")

    def test_hard_reset_keeps_last_stage_without_inventing_a_panic(self):
        emulator = self.emulator(silent_after=1)
        result, events, stage = self.run_controller(emulator, "--set", "a=1", "--set", "b=2",
                                                    timeout="1")
        self.assertEqual(result.returncode, 1)
        self.assertTrue(b"no reboot is inferred" in result.stderr or
                        b"did not acknowledge" in result.stderr, result.stderr)
        self.assertEqual(stage["outcome"], "idle_timeout")
        self.assertEqual(stage["stage"], "efi-loader")
        self.assertIsNone(stage["panic_text"])
        self.assertIn("silence does not establish a panic", stage["conclusion"])

    def test_received_panic_text_is_reported_as_received(self):
        emulator = self.emulator(silent_after=0, panic_after=True)
        result, _, stage = self.run_controller(emulator, "--set", "a=1", timeout="1")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(stage["panic_text"], "emulated loader fault")
        self.assertEqual(stage["conclusion"], "panic text received: emulated loader fault")


class TftpTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "root"
        self.root.mkdir()
        (self.root / "exact").write_bytes(b"x" * 1024)
        (Path(self.tmp.name) / "secret").write_bytes(b"secret")
        (self.root / "link").symlink_to(Path(self.tmp.name) / "secret")
        self.events = []
        self.server = tftp_server.TftpServer(
            self.root, "127.0.0.1", 0, retry_seconds=.2,
            event=lambda name, **f: self.events.append((name, f))).start()

    def tearDown(self):
        self.server.stop()
        self.tmp.cleanup()

    def test_block_boundary_and_default_block_size(self):
        data, size = tftp_server.fetch("127.0.0.1", self.server.port, "exact", blksize=512)
        self.assertEqual((data, size), (b"x" * 1024, 1024))

    def test_traversal_symlink_missing_and_write_are_refused(self):
        for name in ("../secret", "link", "missing", "/"):
            with self.subTest(name=name), self.assertRaises(FileNotFoundError):
                tftp_server.fetch("127.0.0.1", self.server.port, name)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(2)
            sock.sendto(struct.pack("!H", 2) + b"exact\0octet\0", ("127.0.0.1", self.server.port))
            reply, _ = sock.recvfrom(512)
        self.assertEqual(struct.unpack_from("!HH", reply), (5, 2))

    def test_lost_ack_retransmits_the_same_block(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(2)
            sock.sendto(struct.pack("!H", 1) + b"exact\0octet\0", ("127.0.0.1", self.server.port))
            first, tid = sock.recvfrom(1024)
            again, _ = sock.recvfrom(1024)  # no ACK sent: same block again
            self.assertEqual(first, again)
            self.assertEqual(struct.unpack_from("!HH", first), (3, 1))
            sock.sendto(struct.pack("!HH", 4, 1), tid)
            second, _ = sock.recvfrom(1024)
            self.assertEqual(struct.unpack_from("!HH", second), (3, 2))
            sock.sendto(struct.pack("!HH", 4, 2), tid)
            last, _ = sock.recvfrom(1024)
            self.assertEqual(len(last), 4)  # empty final block after 1024 bytes
            sock.sendto(struct.pack("!HH", 4, 3), tid)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and not any(
                e[0] == "tftp_complete" for e in self.events):
            time.sleep(.05)
        self.assertIn("tftp_complete", [e[0] for e in self.events])


class StageTests(unittest.TestCase):
    def test_marker_split_across_chunks_and_milestone_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "stage.json"
            recorder = StageRecorder(str(path), "b")
            recorder.output(b"netdebug-stage: kernel-root")
            self.assertIsNone(json.loads(path.read_text())["earliest_stage"])
            recorder.output(b"conf boot_id=b if=vtnet0\nTrying to mount root from x\n"
                            b"\nmountroot> ")
            state = json.loads(path.read_text())
            self.assertEqual(state["earliest_stage"], "kernel-rootconf")
            self.assertEqual(state["markers"][0]["if"], "vtnet0")
            self.assertEqual(state["stage"], "mountroot_prompt")
            recorder.finish("idle_timeout")
            resumed = StageRecorder(str(path), "b", resume=True)
            self.assertEqual((resumed.state["restarts"], resumed.state["stage"]),
                             (1, "mountroot_prompt"))


if __name__ == "__main__":
    unittest.main()

