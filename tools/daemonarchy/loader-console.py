#!/usr/bin/env python3
"""Approved UDP console for a FreeBSD UEFI loader on a trusted wired network.

--plain uses a public all-zero protocol key: no authentication or encryption.
Keyed mode authenticates packets but does not encrypt traffic. Capture files
retain output/events only.
Exit 3 means the firmware transport closed without an observed kernel session.
--follow-kernel requires a fresh same-boot kernel response; silence never proves
a reboot or panic.
"""

import argparse
import errno
import hmac
import re
import importlib.util
import json
import os
from pathlib import Path
import secrets
import select
import socket
import stat
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netdebug_stage import StageRecorder  # noqa: E402
from tftp_server import TftpServer  # noqa: E402

HEADER = struct.Struct("!4sBBHQ16s")
HELLO, INPUT, OUTPUT, INPUT_ACK, FINISH, CHALLENGE, WELCOME, OUTPUT_ACK = range(1, 9)
KERNEL_WELCOME = 9
ZERO = bytes(16)
MAX_PAYLOAD = 512


def encode(key, session, kind, sequence, payload=b""):
    if len(key) != 32 or len(session) != 16 or len(payload) > MAX_PAYLOAD:
        raise ValueError("invalid packet bounds")
    header = HEADER.pack(b"LDNC", 1, kind, len(payload), sequence, session)
    content = header + payload
    return content + hmac.digest(key, content, "sha256")


def decode(key, data, session=None):
    if not 64 <= len(data) <= 576:
        return None
    magic, version, kind, size, sequence, received = HEADER.unpack_from(data)
    if magic != b"LDNC" or version != 1 or len(data) != 64 + size:
        return None
    if kind in (INPUT, OUTPUT):
        valid = sequence != 0 and 1 <= size <= MAX_PAYLOAD
    elif kind in (INPUT_ACK, OUTPUT_ACK, FINISH):
        valid = sequence != 0 and size == 0
    elif kind == CHALLENGE:
        valid = sequence == 0 and size == 16
    elif kind in (WELCOME, KERNEL_WELCOME):
        valid = sequence == 0 and size == 32
    elif kind == HELLO:
        valid = sequence == 0 and size == 0
    else:
        valid = False
    if not valid or (session is not None and not hmac.compare_digest(received, session)):
        return None
    if not hmac.compare_digest(data[-32:], hmac.digest(key, data[:-32], "sha256")):
        return None
    return kind, sequence, received, data[32:-32]


def read_key(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077:
            raise ValueError("key file must be a private regular file (mode 0600)")
        data = bytearray()
        while len(data) < 67:
            part = os.read(descriptor, 67 - len(data))
            if not part:
                break
            data.extend(part)
        data = bytes(data)
    finally:
        os.close(descriptor)
    if len(data) == 32:
        return data
    if len(data) == 65 and data.endswith(b"\n"):
        data = data[:-1]
    if len(data) != 64:
        raise ValueError("key file must contain 32 raw bytes or 64 hexadecimal digits")
    try:
        key = bytes.fromhex(data.decode("ascii"))
    except (ValueError, UnicodeError) as error:
        raise ValueError("invalid hexadecimal key file") from error
    if len(key) != 32:
        raise ValueError("invalid hexadecimal key length")
    return key


def private_output(path):
    if path is None:
        return None
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    return os.fdopen(descriptor, "wb", buffering=0)


def debugnet_backend():
    path = Path(__file__).with_name("debugnet-console.py")
    spec = importlib.util.spec_from_file_location("daemonarchy_debugnet_console", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


NAME = re.compile(r"^[A-Za-z0-9_.-]{1,64}$")
# The loader tokenizer expands $ and honours quotes and backslashes; refuse
# them, and whitespace, so a value can never become another command.
VALUE = re.compile(r"^[!#%&'()*+,./0-9:<=>?@A-Z\[\]^_`a-z{|}~-]{1,200}$")


def strip_prompts(text):
    """Remove the loader's interactive "OK " prompts from captured output."""
    lines = []
    for line in text.replace("\r", "").split("\n"):
        while line.startswith("OK "):
            line = line[3:]
        if line.strip() not in ("", "OK"):
            lines.append(line)
    return "\n".join(lines)


def loader_operations(args, target):
    """Compile D1 options into standard loader commands, in a fixed order."""
    ops = []

    def value(text, what):
        if not VALUE.match(text) or '"' in text:
            raise ValueError(f"{what} contains characters the loader would reinterpret")
        return text

    if args.tftp_root is not None:
        server = args.tftp_address or args.bind
        if server in ("0.0.0.0", ""):
            raise ValueError("--tftp-root needs --tftp-address or a specific --bind address")
        ops += [("set", f"set net.static.ip={value(args.static_ip or target, 'static IP')}"),
                ("set", f"set net.static.rootpath=tftp://{server}:{args.tftp_port}/")]
        if args.static_netmask:
            ops.append(("set", f"set net.static.netmask={value(args.static_netmask, 'netmask')}"))
    for item in args.set:
        name, sep, text = item.partition("=")
        if not sep or not NAME.match(name):
            raise ValueError("--set needs NAME=VALUE with a plain variable name")
        ops.append(("set", f"set {name}={value(text, '--set value')}"))
    for name in args.unset:
        if not NAME.match(name):
            raise ValueError("--unset needs a plain variable name")
        ops.append(("unset", f"unset {name}"))
    for name in args.show:
        if not NAME.match(name):
            raise ValueError("--show needs a plain variable name")
        ops.append(("show", f"show {name}"))
    if args.kernel:
        ops += [("unload", "unload"), ("load", f"load {value(args.kernel, 'kernel path')}")]
    for module in args.module:
        ops.append(("load", f"load {value(module, 'module path')}"))
    for script in args.lua:
        ops.append(("include", f"include {value(script, 'Lua path')}"))
    ops += [("command", line) for line in args.command]
    if args.stop:
        ops.append(("stop", "netdebug stop"))
    return ops


class Console:
    def __init__(self, target, port, key, *, bind="0.0.0.0", timeout=30, output=None, events=None,
                 stage=None):
        self.key = key
        self.stage = stage
        self.event_lock = threading.Lock()
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind((bind, 0))
        self.socket.connect((target, port))
        self.socket.setblocking(False)
        self.timeout = timeout
        self.output = output
        self.events = events
        self.session = None
        self.boot = None
        self.input_sequence = 1
        self.output_sequence = 1
        self.last_output = None
        self.received = bytearray()
        self.handoff = False
        self.phase = "firmware"

    def close(self):
        self.socket.close()

    def event(self, kind, **fields):
        # The TFTP server reports from its own threads.
        with self.event_lock:
            self._event(kind, **fields)

    def _event(self, kind, **fields):
        if self.events is not None:
            self.events.write((json.dumps({"event": kind, "monotonic": time.monotonic(), **fields}, sort_keys=True) + "\n").encode())

    def _send_retryable(self, data):
        """Send, treating an unreachable target as a lost packet.

        Before the target answers ARP the host reports EHOSTDOWN or
        EHOSTUNREACH; the caller's retry and deadline already bound this.
        """
        try:
            self.socket.send(data)
        except (ConnectionRefusedError, OSError) as error:
            if not isinstance(error, ConnectionRefusedError) and error.errno not in (
                    errno.EHOSTDOWN, errno.EHOSTUNREACH, errno.ENETDOWN):
                raise
            self.event("send_deferred", errno=error.errno)

    def receive(self, seconds):
        if not select.select([self.socket], [], [], max(0, seconds))[0]:
            return None
        data = self.socket.recv(2048)
        value = decode(self.key, data, self.session)
        if value is None:
            self.event("packet_refused")
            return None
        kind, sequence, _, payload = value
        if kind == OUTPUT:
            if sequence == self.output_sequence:
                sys.stdout.buffer.write(payload)
                sys.stdout.buffer.flush()
                if self.output is not None:
                    self.output.write(payload)
                if self.stage is not None:
                    self.stage.output(payload)
                self.received.extend(payload)
                if len(self.received) > 65536:
                    del self.received[:-65536]
                self.last_output = data
                self.output_sequence += 1
            elif sequence != self.output_sequence - 1 or self.last_output != data:
                self.event("output_order_refused", sequence=sequence)
                return None
            self.socket.send(encode(self.key, self.session, OUTPUT_ACK, sequence))
        elif kind == FINISH:
            self.handoff = True
            if self.stage is not None:
                self.stage.stage("firmware_handoff" if self.phase == "firmware"
                                 else "kernel_transport_closed")
            self.event("firmware_handoff_started" if self.phase == "firmware" else "kernel_transport_closed",
                       sequence=sequence, output_gap=sequence != self.output_sequence,
                       kernel_observed=self.phase == "kernel")
        return value

    def pair(self, *, kernel=False):
        expected_boot = self.boot if kernel else None
        previous_session = self.session
        if kernel and (not self.handoff or expected_boot is None or self.phase != "firmware"):
            raise ValueError("kernel pairing requires an observed firmware handoff and boot identity")
        challenge = secrets.token_bytes(16)
        request = encode(self.key, ZERO, CHALLENGE, 0, challenge)
        end = time.monotonic() + self.timeout
        retry = 0
        while time.monotonic() < end:
            if time.monotonic() >= retry:
                self._send_retryable(request)
                retry = time.monotonic() + 0.5
            if not select.select([self.socket], [], [], max(0, min(0.1, end - time.monotonic())))[0]:
                continue
            try:
                data = self.socket.recv(2048)
            except ConnectionRefusedError:
                continue
            value = decode(self.key, data)
            expected_kind = KERNEL_WELCOME if kernel else WELCOME
            if value is None or value[0] != expected_kind or value[3][:16] != challenge or value[2] == ZERO:
                self.event("pairing_packet_refused")
                continue
            if kernel and (value[3][16:] != expected_boot or value[2] == previous_session):
                self.event("kernel_identity_refused")
                continue
            self.session, self.boot = value[2], value[3][16:]
            self.input_sequence = self.output_sequence = 1
            self.last_output = None
            self.received.clear()
            self.handoff = False
            self.phase = "kernel" if kernel else "firmware"
            if self.stage is not None:
                self.stage.state["boot_id"] = self.boot.hex()
                self.stage.stage("kernel_admitted" if kernel else "firmware_paired")
            self.event("kernel_connected" if kernel else "paired", session=self.session.hex(),
                       boot_id=self.boot.hex(), kernel_observed=kernel)
            return
        if kernel:
            self.event("kernel_connection_unobserved", boot_id=expected_boot.hex(), kernel_observed=False)
        raise TimeoutError("no authenticated kernel response for this boot" if kernel else
                           "no authenticated response to the fresh pairing challenge")

    def send_line(self, line):
        encoded = line.encode("ascii")
        if len(encoded) > 255 or any(c != 9 and not 32 <= c <= 126 for c in encoded):
            raise ValueError("commands require 0..255 printable ASCII bytes (tabs allowed)")
        sequence = self.input_sequence
        packet = encode(self.key, self.session, INPUT, sequence, encoded + b"\n")
        end = time.monotonic() + self.timeout
        retry = 0
        while time.monotonic() < end:
            if time.monotonic() >= retry:
                self._send_retryable(packet)
                retry = time.monotonic() + 0.5
            value = self.receive(min(0.1, end - time.monotonic()))
            if self.handoff:
                return
            if value is not None and value[0] == INPUT_ACK and value[1] == sequence:
                self.input_sequence += 1
                return
        self.event("input_ack_timeout", sequence=sequence)
        raise TimeoutError(f"{self.phase} console did not acknowledge command admission")

    def _await_marker(self, marker):
        end = time.monotonic() + self.timeout
        while marker.encode() not in self.received and not self.handoff:
            if time.monotonic() >= end:
                self.event("command_completion_unobserved")
                raise TimeoutError("command completion was not observed; no reboot is inferred")
            self.receive(min(0.1, end - time.monotonic()))

    def command(self, line, *, capture=False):
        """Run one loader line; with capture, return exactly its output."""
        # Kernel prompts (mountroot/DDB) do not run loader echo commands.
        if self.phase == "kernel":
            self.send_line(line)
            return None
        if capture:
            # Bracket the command: output queued earlier ends at this marker.
            start = "LDNC-SYNC-" + secrets.token_hex(16)
            self.received.clear()
            self.send_line("echo " + start)
            self._await_marker(start)
            if self.handoff:
                return None
            del self.received[:self.received.find(start.encode()) + len(start)]
            del self.received[:1 if self.received[:1] == b"\n" else 0]
        else:
            self.received.clear()
        self.send_line(line)
        if self.handoff:
            return None
        marker = "LDNC-DONE-" + secrets.token_hex(16)
        self.send_line("echo " + marker)
        self._await_marker(marker)
        if self.handoff:
            return None
        self.event("command_completed")
        text = bytes(self.received[:self.received.find(marker.encode())])
        return strip_prompts(text.decode("ascii", "replace")) if capture else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", nargs="?")
    parser.add_argument("--port", type=int, default=6668)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--key-file", type=Path)
    parser.add_argument("--plain", action="store_true",
                        help="explicit unauthenticated debug mode; trusted wired network only")
    parser.add_argument("--make-key", type=Path)
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=30,
                        help="operation deadline and --follow-kernel idle deadline in seconds")
    parser.add_argument("--follow-kernel", action="store_true",
                        help="keep captures open and admit a fresh same-boot kernel session after firmware closes")
    parser.add_argument("--kernel-transport", choices=("ldnc", "debugnet"), default="ldnc",
                        help="kernel wire protocol; debugnet needs --plain and --output")
    parser.add_argument("--output", type=Path, help="new private output-only capture")
    parser.add_argument("--events", type=Path, help="new private metadata-only event capture")
    parser.add_argument("--last-stage", type=Path,
                        help="last received stage record; retained after silence or reset")
    loader = parser.add_argument_group("standard loader operations, run before --command")
    loader.add_argument("--set", action="append", default=[], metavar="NAME=VALUE")
    loader.add_argument("--unset", action="append", default=[], metavar="NAME")
    loader.add_argument("--show", action="append", default=[], metavar="NAME",
                        help="record the value in a variable_shown event")
    loader.add_argument("--kernel", metavar="PATH",
                        help="unload, then load this kernel (disk path or net0:/name)")
    loader.add_argument("--module", action="append", default=[], metavar="PATH")
    loader.add_argument("--lua", action="append", default=[], metavar="PATH",
                        help="run a script with the loader's own Lua include")
    loader.add_argument("--tftp-root", type=Path,
                        help="serve this directory read-only over TFTP and point the loader's "
                             "net device at it with static net.static.* settings")
    loader.add_argument("--tftp-port", type=int, default=6969)
    loader.add_argument("--tftp-address", help="controller address the loader reaches")
    loader.add_argument("--static-ip", help="loader address for net.static.ip (default: target)")
    loader.add_argument("--static-netmask")
    loader.add_argument("--stop", action="store_true",
                        help="finish with netdebug stop; the console is revoked")
    args = parser.parse_args()
    if args.make_key:
        stream = private_output(args.make_key)
        with stream:
            stream.write(secrets.token_hex(32).encode() + b"\n")
        return 0
    if (not args.target or args.plain == bool(args.key_file) or
            not 0 < args.timeout <= 3600 or not 1 <= args.port <= 65535):
        parser.error("target, exactly one of --plain/--key-file, a valid port and a 0..3600 second timeout are required")
    if args.kernel_transport == "debugnet" and (not args.follow_kernel or
            not args.plain or args.output is None):
        parser.error("debugnet continuation needs --follow-kernel, --plain and --output")
    output = events = None
    client = None
    kernel_listener = None
    tftp = None
    stage = None
    outcome = "error"
    try:
        operations = loader_operations(args, args.target)
        output = private_output(args.output)
        events = private_output(args.events)
        if args.last_stage is not None:
            if args.last_stage.exists():
                raise ValueError("--last-stage must name a new file")
            stage = StageRecorder(str(args.last_stage))
        if args.kernel_transport == "debugnet":
            kernel_listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            kernel_listener.bind((args.bind, debugnet_backend().PORT))
        client = Console(args.target, args.port, bytes(32) if args.plain else read_key(args.key_file), bind=args.bind,
                         timeout=args.timeout, output=output, events=events, stage=stage)
        if args.tftp_root is not None:
            tftp = TftpServer(args.tftp_root, args.tftp_address or args.bind, args.tftp_port,
                              event=client.event).start()
        client.pair()
        for kind, line in operations:
            if kind == "stop":
                # Admission is acknowledged before the loader revokes the console.
                client.send_line(line)
                client.event("console_stopped")
                outcome = "stopped"
                return 0
            text = client.command(line, capture=kind != "command")
            if kind == "show" and text is not None:
                client.event("variable_shown", name=line.split()[1], output=text.strip())
            elif text is not None:
                client.event("loader_operation", operation=kind, command=line,
                             output=text.strip()[-1024:])
            if client.handoff:
                break
        if args.follow_kernel:
            # EOF only ends keyboard input; durable network capture continues.
            stdin_open = True
            idle_end = time.monotonic() + args.timeout
            while True:
                if client.handoff:
                    if client.phase == "kernel":
                        break
                    if args.kernel_transport == "debugnet":
                        boot = client.boot.hex()

                        def admitted(peer):
                            client.phase = "kernel"
                            client.handoff = False
                            client.event("kernel_connected", boot_id=boot,
                                         transport="debugnet", kernel_observed=True)

                        try:
                            debugnet_backend().serve(args.bind, boot, args.timeout,
                                                     output, listener=kernel_listener,
                                                     on_admit=admitted, stage=stage,
                                                     finish_stage=False)
                        except TimeoutError:
                            client.event("capture_idle_timeout", phase=client.phase,
                                         kernel_observed=client.phase == "kernel")
                            raise
                        kernel_listener = None
                        client.event("kernel_transport_closed", phase="kernel",
                                     kernel_observed=True)
                        outcome = "finished"
                        break
                    client.pair(kernel=True)
                    idle_end = time.monotonic() + args.timeout
                value = client.receive(min(0.1, max(0, idle_end - time.monotonic())))
                if value is not None:
                    idle_end = time.monotonic() + args.timeout
                if client.handoff:
                    continue
                if stdin_open and select.select([sys.stdin], [], [], 0)[0]:
                    line = sys.stdin.readline()
                    if line:
                        client.command(line.rstrip("\r\n"))
                        idle_end = time.monotonic() + args.timeout
                    else:
                        stdin_open = False
                if time.monotonic() >= idle_end:
                    client.event("capture_idle_timeout", phase=client.phase,
                                 kernel_observed=client.phase == "kernel")
                    raise TimeoutError("network capture idle deadline expired; no reboot is inferred")
        elif not args.command:
            # Keep receiving output while the operator edits a local input line.
            while not client.handoff:
                client.receive(0.1)
                if select.select([sys.stdin], [], [], 0)[0]:
                    line = sys.stdin.readline()
                    if not line:
                        break
                    client.command(line.rstrip("\r\n"))
        if client.handoff and client.phase == "firmware":
            outcome = "firmware_handoff"
            return 3
        outcome = "finished" if client.handoff else "operator_closed"
        return 0
    except TimeoutError:
        outcome = "idle_timeout"
        raise
    except KeyboardInterrupt:
        outcome = "interrupted"
        if client is not None:
            client.event("controller_stopped", phase=client.phase,
                         kernel_observed=client.phase == "kernel")
        return 130
    finally:
        if stage is not None:
            stage.finish(outcome)
        if tftp is not None:
            tftp.stop()
        if kernel_listener is not None:
            kernel_listener.close()
        if client is not None:
            client.close()
        if output is not None:
            output.close()
        if events is not None:
            events.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, TimeoutError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
