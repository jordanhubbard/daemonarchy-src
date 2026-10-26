#!/usr/bin/env python3
"""Bounded plain debugnet-to-GDB bridge for a trusted diagnostic link."""

import argparse
import json
import select
import socket
import struct
import sys
import time

HEADER = struct.Struct("!IIQII")
ACK = struct.Struct("!I")
HERALD, FINISHED, DATA = 1, 2, 3
NETGDB_PROTO_V1 = 0x2515F095
HERALD_PORT = 20025
GDB_PORT = HERALD_PORT + 1
MAX_QUEUED_OUTPUT = 65536


def serve(bind, expected_peer, timeout, capture, events):
    peer = None
    gdb = None
    last_rx = 0
    next_tx = 1
    pending = None
    queued = bytearray()
    deadline = time.monotonic() + timeout

    def event(name, **fields):
        events.write(json.dumps({"event": name, **fields}, sort_keys=True) + "\n")
        events.flush()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, \
            socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        udp.bind((bind, HERALD_PORT))
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((bind, GDB_PORT))
        listener.listen(1)
        udp.setblocking(False)
        listener.setblocking(False)
        try:
            while time.monotonic() < deadline:
                now = time.monotonic()
                if pending is not None and now >= pending[2]:
                    if pending[3] >= max(5, min(1200, int(timeout / .5))):
                        raise TimeoutError("kernel did not acknowledge GDB input")
                    udp.sendto(pending[1], peer)
                    pending = (pending[0], pending[1], now + .5,
                               pending[3] + 1)
                readers = [udp, listener]
                if gdb is not None and peer is not None and pending is None:
                    readers.append(gdb)
                ready, _, _ = select.select(readers, [], [], .1)
                if listener in ready:
                    candidate, _ = listener.accept()
                    candidate.settimeout(1)
                    if gdb is not None:
                        candidate.close()
                    else:
                        gdb = candidate
                        event("gdb_connected")
                        if queued:
                            gdb.sendall(queued)
                            queued.clear()
                if gdb is not None and gdb in ready:
                    payload = gdb.recv(512)
                    if not payload:
                        if peer is not None:
                            udp.sendto(HEADER.pack(FINISHED, next_tx, 0, 0, 0), peer)
                        event("gdb_disconnected")
                        return 0
                    wire = HEADER.pack(DATA, next_tx, 0, len(payload), 0) + payload
                    udp.sendto(wire, peer)
                    pending = (next_tx, wire, time.monotonic() + .5, 0)
                    event("gdb_input_sent", bytes=len(payload), sequence=next_tx)
                if udp not in ready:
                    continue
                packet, addr = udp.recvfrom(65536)
                if addr[0] != expected_peer or (peer is not None and addr != peer):
                    continue
                if peer is not None and len(packet) == ACK.size:
                    seq, = ACK.unpack(packet)
                    if pending is not None and seq == pending[0]:
                        pending = None
                        next_tx += 1
                        deadline = time.monotonic() + timeout
                        event("gdb_input_acked", sequence=seq)
                    continue
                if len(packet) < HEADER.size:
                    continue
                kind, seq, offset, length, aux2 = HEADER.unpack_from(packet)
                if length != len(packet) - HEADER.size:
                    continue
                payload = packet[HEADER.size:]
                if peer is None:
                    if kind != HERALD or aux2 != NETGDB_PROTO_V1:
                        continue
                    peer = addr
                    last_rx = seq
                    udp.sendto(ACK.pack(seq), peer)
                    deadline = time.monotonic() + timeout
                    event("kernel_connected", peer=addr[0], protocol=aux2)
                    continue
                if kind == FINISHED:
                    udp.sendto(ACK.pack(seq), peer)
                    event("kernel_finished")
                    return 0
                if kind != DATA or seq > last_rx + 1:
                    continue
                if seq > last_rx:
                    if len(queued) + len(payload) > MAX_QUEUED_OUTPUT:
                        raise BufferError("GDB output queue is full")
                    capture.write(payload)
                    capture.flush()
                    if gdb is None:
                        queued.extend(payload)
                    else:
                        gdb.sendall(payload)
                    last_rx = seq
                    deadline = time.monotonic() + timeout
                    event("kernel_output", bytes=len(payload), offset=offset,
                          sequence=seq)
                udp.sendto(ACK.pack(seq), peer)
            raise TimeoutError("kernel or GDB session idle or missing")
        finally:
            if gdb is not None:
                gdb.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--peer", required=True)
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--capture", required=True)
    parser.add_argument("--events", required=True)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    try:
        with open(args.capture, "xb", buffering=0) as capture, \
                open(args.events, "x", encoding="utf-8") as events:
            return serve(args.bind, args.peer, args.timeout, capture, events)
    except (OSError, BufferError, TimeoutError) as exc:
        print(exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
