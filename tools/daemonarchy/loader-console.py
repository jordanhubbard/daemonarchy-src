#!/usr/bin/env python3
"""Approved UDP console for a FreeBSD UEFI loader on a trusted wired network.

Authentication does not encrypt traffic. Capture files retain output/events only.
Exit 3 means the firmware transport closed before kernel handoff; kernel state
must be observed independently.
"""

import argparse
import hmac
import json
import os
from pathlib import Path
import secrets
import select
import socket
import stat
import struct
import sys
import time

HEADER = struct.Struct("!4sBBHQ16s")
HELLO, INPUT, OUTPUT, INPUT_ACK, FINISH, CHALLENGE, WELCOME, OUTPUT_ACK = range(1, 9)
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
    elif kind == WELCOME:
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


class Console:
    def __init__(self, target, port, key, *, bind="0.0.0.0", timeout=30, output=None, events=None):
        self.key = key
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

    def close(self):
        self.socket.close()

    def event(self, kind, **fields):
        if self.events is not None:
            self.events.write((json.dumps({"event": kind, "monotonic": time.monotonic(), **fields}, sort_keys=True) + "\n").encode())

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
            self.event("firmware_handoff_started", sequence=sequence,
                       output_gap=sequence != self.output_sequence, kernel_observed=False)
        return value

    def pair(self):
        challenge = secrets.token_bytes(16)
        request = encode(self.key, ZERO, CHALLENGE, 0, challenge)
        end = time.monotonic() + self.timeout
        retry = 0
        while time.monotonic() < end:
            if time.monotonic() >= retry:
                self.socket.send(request)
                retry = time.monotonic() + 0.5
            if not select.select([self.socket], [], [], max(0, min(0.1, end - time.monotonic())))[0]:
                continue
            try:
                data = self.socket.recv(2048)
            except ConnectionRefusedError:
                continue
            value = decode(self.key, data)
            if value is None or value[0] != WELCOME or value[3][:16] != challenge:
                self.event("pairing_packet_refused")
                continue
            self.session, self.boot = value[2], value[3][16:]
            self.event("paired", session=self.session.hex(), boot_id=self.boot.hex())
            return
        raise TimeoutError("no authenticated response to the fresh pairing challenge")

    def send_line(self, line):
        encoded = line.encode("ascii")
        if not encoded or len(encoded) > 255 or any(c != 9 and not 32 <= c <= 126 for c in encoded):
            raise ValueError("commands require 1..255 printable ASCII bytes (tabs allowed)")
        sequence = self.input_sequence
        packet = encode(self.key, self.session, INPUT, sequence, encoded + b"\n")
        end = time.monotonic() + self.timeout
        retry = 0
        while time.monotonic() < end:
            if time.monotonic() >= retry:
                self.socket.send(packet)
                retry = time.monotonic() + 0.5
            value = self.receive(min(0.1, end - time.monotonic()))
            if self.handoff:
                return
            if value is not None and value[0] == INPUT_ACK and value[1] == sequence:
                self.input_sequence += 1
                return
        self.event("input_ack_timeout", sequence=sequence)
        raise TimeoutError("loader did not acknowledge command admission")

    def command(self, line):
        self.send_line(line)
        if self.handoff:
            return
        marker = "LDNC-DONE-" + secrets.token_hex(16)
        self.received.clear()
        self.send_line("echo " + marker)
        end = time.monotonic() + self.timeout
        while marker.encode() not in self.received and not self.handoff:
            if time.monotonic() >= end:
                self.event("command_completion_unobserved")
                raise TimeoutError("command completion was not observed; no reboot is inferred")
            self.receive(min(0.1, end - time.monotonic()))
        if not self.handoff:
            self.event("command_completed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", nargs="?")
    parser.add_argument("--port", type=int, default=6668)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--key-file", type=Path)
    parser.add_argument("--make-key", type=Path)
    parser.add_argument("--command", action="append", default=[])
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", type=Path, help="new private output-only capture")
    parser.add_argument("--events", type=Path, help="new private metadata-only event capture")
    args = parser.parse_args()
    if args.make_key:
        stream = private_output(args.make_key)
        with stream:
            stream.write(secrets.token_hex(32).encode() + b"\n")
        return 0
    if not args.target or not args.key_file or not 0 < args.timeout <= 3600 or not 1 <= args.port <= 65535:
        parser.error("target, --key-file, a valid port and a 0..3600 second timeout are required")
    output = events = None
    client = None
    try:
        output = private_output(args.output)
        events = private_output(args.events)
        client = Console(args.target, args.port, read_key(args.key_file), bind=args.bind,
                         timeout=args.timeout, output=output, events=events)
        client.pair()
        if args.command:
            for line in args.command:
                client.command(line)
                if client.handoff:
                    break
        else:
            # Keep receiving output while the operator edits a local input line.
            while not client.handoff:
                client.receive(0.1)
                if select.select([sys.stdin], [], [], 0)[0]:
                    line = sys.stdin.readline()
                    if not line:
                        break
                    client.command(line.rstrip("\r\n"))
        return 3 if client.handoff else 0
    finally:
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
