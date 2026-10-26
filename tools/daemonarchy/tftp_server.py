"""Bounded read-only TFTP server for the loader debug link.

Serves regular files under one root to the FreeBSD loader's libsa TFTP
client (RFC 1350 with the RFC 2347 option negotiation for blksize and
tsize that libsa requests).  Writes, path traversal, symlinks out of the
root and oversized files are refused.  Each transfer uses its own socket
(transfer identifier) and is bounded by retries and an overall deadline.
"""

import socket
import struct
import threading
import time
from pathlib import Path

RRQ, WRQ, DATA, ACK, ERROR, OACK = 1, 2, 3, 4, 5, 6
MAX_FILE = 512 << 20
MAX_TRANSFERS = 4
MIN_BLKSIZE, MAX_BLKSIZE = 8, 1428  # keep each datagram inside a 1500 MTU


class TftpServer:
    def __init__(self, root, bind, port, *, event=None, retries=5, retry_seconds=1.0,
                 transfer_seconds=300):
        self.root = Path(root).resolve(strict=True)
        if not self.root.is_dir():
            raise ValueError("TFTP root must be a directory")
        self.event = event or (lambda name, **fields: None)
        self.retries, self.retry_seconds = retries, retry_seconds
        self.transfer_seconds = transfer_seconds
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((bind, port))
        self.sock.settimeout(.2)
        self.bind = bind
        self.active = threading.BoundedSemaphore(MAX_TRANSFERS)
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.workers = []

    @property
    def port(self):
        return self.sock.getsockname()[1]

    def start(self):
        self.thread.start()
        return self

    def stop(self):
        self.stopping.set()
        self.thread.join(2)
        for worker in self.workers:
            worker.join(2)
        self.sock.close()

    def _serve(self):
        while not self.stopping.is_set():
            try:
                packet, peer = self.sock.recvfrom(1024)
            except socket.timeout:
                continue
            except OSError:
                return
            worker = threading.Thread(target=self._request, args=(packet, peer), daemon=True)
            self.workers = [w for w in self.workers if w.is_alive()] + [worker]
            worker.start()

    def _resolve(self, name):
        name = name.lstrip("/")
        if not name or "\\" in name or any(part in ("", ".", "..")
                                           for part in name.split("/")):
            return None
        path = (self.root / name)
        try:
            real = path.resolve(strict=True)
        except OSError:
            return None
        if self.root not in real.parents or not real.is_file():
            return None
        return real

    @staticmethod
    def _parse(packet):
        fields = packet[2:].split(b"\0")
        if len(fields) < 3 or fields[-1] != b"":
            return None
        fields = [f.decode("ascii", "replace") for f in fields[:-1]]
        name, mode, options = fields[0], fields[1].lower(), fields[2:]
        if len(options) % 2:
            return None
        return name, mode, {options[i].lower(): options[i + 1]
                            for i in range(0, len(options), 2)}

    def _request(self, packet, peer):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as tid:
            tid.bind((self.bind, 0))
            tid.settimeout(self.retry_seconds)

            def error(code, text):
                try:
                    tid.sendto(struct.pack("!HH", ERROR, code) + text.encode() + b"\0", peer)
                except OSError:
                    pass
                self.event("tftp_refused", peer=peer[0], code=code, reason=text)

            if len(packet) < 4:
                return
            opcode, = struct.unpack_from("!H", packet)
            if opcode == WRQ:
                return error(2, "read-only server")
            parsed = self._parse(packet) if opcode == RRQ else None
            if parsed is None:
                return error(4, "illegal TFTP operation")
            name, mode, options = parsed
            if mode != "octet":
                return error(0, "only octet mode is served")
            path = self._resolve(name)
            if path is None:
                return error(1, "file not found")
            size = path.stat().st_size
            if size > MAX_FILE:
                return error(3, "file exceeds the served size bound")
            if not self.active.acquire(blocking=False):
                return error(0, "too many transfers")
            try:
                self._send(tid, peer, name, path, size, options)
            finally:
                self.active.release()

    def _exchange(self, tid, peer, packet, expect_block):
        """Send `packet` until the peer ACKs `expect_block`; return success."""
        for _ in range(self.retries + 1):
            try:
                tid.sendto(packet, peer)
            except OSError as error:
                # A transient host error (EHOSTDOWN while the loader is busy
                # and ARP lapses) is a lost packet: retry within the bound.
                self.event("tftp_send_error", peer=peer[0], errno=error.errno)
                time.sleep(self.retry_seconds)
                continue
            end = time.monotonic() + self.retry_seconds
            while time.monotonic() < end:
                try:
                    reply, source = tid.recvfrom(1024)
                except socket.timeout:
                    break
                if source != peer:
                    try:
                        tid.sendto(struct.pack("!HH", ERROR, 5) + b"unknown transfer ID\0",
                                   source)
                    except OSError:
                        pass
                    continue
                if len(reply) >= 4:
                    opcode, block = struct.unpack_from("!HH", reply)
                    if opcode == ACK and block == expect_block:
                        return True
                    if opcode == ERROR:
                        return False
        return False

    def _send(self, tid, peer, name, path, size, options):
        blksize = 512
        accepted = {}
        if "blksize" in options:
            try:
                blksize = max(MIN_BLKSIZE, min(MAX_BLKSIZE, int(options["blksize"])))
                accepted["blksize"] = str(blksize)
            except ValueError:
                pass
        if "tsize" in options:
            accepted["tsize"] = str(size)
        start = time.monotonic()
        self.event("tftp_request", peer=peer[0], file=name, size=size, blksize=blksize)
        if accepted:
            oack = struct.pack("!H", OACK) + b"".join(
                k.encode() + b"\0" + v.encode() + b"\0" for k, v in accepted.items())
            if not self._exchange(tid, peer, oack, 0):
                self.event("tftp_failed", peer=peer[0], file=name, block=0)
                return
        block = 0
        sent = 0
        with open(path, "rb") as handle:
            while True:
                if time.monotonic() - start > self.transfer_seconds:
                    self.event("tftp_failed", peer=peer[0], file=name, reason="deadline")
                    return
                chunk = handle.read(blksize)
                block = (block + 1) & 0xffff
                if not self._exchange(tid, peer, struct.pack("!HH", DATA, block) + chunk,
                                      block):
                    self.event("tftp_failed", peer=peer[0], file=name, block=block)
                    return
                sent += len(chunk)
                if len(chunk) < blksize:
                    break
        self.event("tftp_complete", peer=peer[0], file=name, bytes=sent,
                   seconds=round(time.monotonic() - start, 3))


def fetch(server, port, name, *, blksize=1428, timeout=2.0):
    """Minimal libsa-like client used by tests: RRQ with blksize and tsize."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.sendto(struct.pack("!H", RRQ) + name.encode() + b"\0octet\0blksize\0" +
                    str(blksize).encode() + b"\0tsize\00\0", (server, port))
        data = bytearray()
        size = None
        expected = 1
        while True:
            packet, tid = sock.recvfrom(65536)
            opcode, = struct.unpack_from("!H", packet)
            if opcode == ERROR:
                raise FileNotFoundError(packet[4:-1].decode())
            if opcode == OACK:
                fields = packet[2:].split(b"\0")[:-1]
                options = dict(zip(fields[::2], fields[1::2]))
                blksize = int(options.get(b"blksize", b"512"))
                size = int(options[b"tsize"]) if b"tsize" in options else None
                sock.sendto(struct.pack("!HH", ACK, 0), tid)
                continue
            block, = struct.unpack_from("!H", packet, 2)
            if block == expected:
                data.extend(packet[4:])
                expected = (expected + 1) & 0xffff
            sock.sendto(struct.pack("!HH", ACK, block), tid)
            if block == (expected - 1) & 0xffff and len(packet) - 4 < blksize:
                return bytes(data), size


__all__ = ["TftpServer", "fetch"]
