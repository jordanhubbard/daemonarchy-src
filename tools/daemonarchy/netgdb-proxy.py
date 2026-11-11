#!/usr/bin/env python3
"""Bounded plain debugnet-to-GDB bridge for a trusted diagnostic link."""

import argparse
import hashlib
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
COUNTERS = ("malformed", "foreign_peer", "unexpected", "duplicate_output",
            "sequence_reuse", "out_of_order", "input_retransmit", "stale_ack",
            "reherald", "output_bytes", "input_bytes", "rejected_gdb_clients")


def serve(bind, expected_peer, timeout, capture, events):
    stats = dict.fromkeys(COUNTERS, 0)
    peer = None
    gdb = None
    last_rx = 0
    last_digest = None
    next_tx = 1
    pending = None
    queued = bytearray()
    deadline = time.monotonic() + timeout
    outcome = "error"

    def event(name, **fields):
        events.write(json.dumps({"event": name, "time": time.time(), **fields},
                                sort_keys=True) + "\n")
        events.flush()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp, \
            socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        try:
            udp.bind((bind, HERALD_PORT))
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((bind, GDB_PORT))
            listener.listen(1)
            udp.setblocking(False)
            listener.setblocking(False)
            # Pending GDB input is bounded by its own retransmission limit.
            while pending is not None or time.monotonic() < deadline:
                now = time.monotonic()
                if pending is not None and now >= pending[2]:
                    if pending[3] >= max(5, min(1200, int(timeout / .5))):
                        event("gdb_input_unacknowledged", sequence=pending[0])
                        raise TimeoutError("kernel did not acknowledge GDB input")
                    udp.sendto(pending[1], peer)
                    stats["input_retransmit"] += 1
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
                        stats["rejected_gdb_clients"] += 1
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
                        outcome = "gdb_disconnected"
                        return 0
                    wire = HEADER.pack(DATA, next_tx, 0, len(payload), 0) + payload
                    udp.sendto(wire, peer)
                    pending = (next_tx, wire, time.monotonic() + .5, 0)
                    stats["input_bytes"] += len(payload)
                    event("gdb_input_sent", bytes=len(payload), sequence=next_tx)
                if udp not in ready:
                    continue
                packet, addr = udp.recvfrom(65536)
                if addr[0] != expected_peer or (peer is not None and addr != peer):
                    stats["foreign_peer"] += 1
                    continue
                if peer is not None and len(packet) == ACK.size:
                    seq, = ACK.unpack(packet)
                    if pending is not None and seq == pending[0]:
                        pending = None
                        next_tx += 1
                        deadline = time.monotonic() + timeout
                        event("gdb_input_acked", sequence=seq)
                    else:
                        stats["stale_ack"] += 1
                    continue
                if len(packet) < HEADER.size:
                    stats["malformed"] += 1
                    continue
                kind, seq, offset, length, aux2 = HEADER.unpack_from(packet)
                if length != len(packet) - HEADER.size:
                    stats["malformed"] += 1
                    continue
                payload = packet[HEADER.size:]
                if kind == HERALD and aux2 == NETGDB_PROTO_V1:
                    # A herald on an admitted session is a new kernel netgdb pcb.
                    if peer is not None:
                        stats["reherald"] += 1
                        if pending is not None:
                            event("gdb_input_abandoned", sequence=pending[0])
                            pending = None
                            next_tx += 1
                    peer = addr
                    last_rx, last_digest = seq, None
                    udp.sendto(ACK.pack(seq), peer)
                    deadline = time.monotonic() + timeout
                    event("kernel_connected", peer=addr[0], protocol=aux2,
                          reherald=stats["reherald"] > 0)
                    continue
                if peer is None:
                    stats["unexpected"] += 1
                    continue
                if kind == FINISHED:
                    udp.sendto(ACK.pack(seq), peer)
                    event("kernel_finished")
                    outcome = "kernel_finished"
                    return 0
                if kind != DATA:
                    stats["unexpected"] += 1
                    continue
                if seq > last_rx + 1:
                    # Not ACKed: debugnet retransmits it after the missing one.
                    stats["out_of_order"] += 1
                    continue
                digest = hashlib.sha256(payload).hexdigest()
                # debugnet keeps its sequence after a timed-out send, so new
                # bytes under the last sequence are output, not a duplicate.
                if seq > last_rx or (seq == last_rx and digest != last_digest):
                    if len(queued) + len(payload) > MAX_QUEUED_OUTPUT:
                        raise BufferError("GDB output queue is full")
                    if seq == last_rx:
                        stats["sequence_reuse"] += 1
                        event("kernel_sequence_reuse", sequence=seq)
                    capture.write(payload)
                    capture.flush()
                    if gdb is None:
                        queued.extend(payload)
                    else:
                        gdb.sendall(payload)
                    last_rx, last_digest = seq, digest
                    stats["output_bytes"] += len(payload)
                    deadline = time.monotonic() + timeout
                    event("kernel_output", bytes=len(payload), offset=offset,
                          sequence=seq)
                else:
                    stats["duplicate_output"] += 1
                udp.sendto(ACK.pack(seq), peer)
            outcome = "idle_timeout"
            raise TimeoutError("kernel or GDB session idle or missing")
        finally:
            if gdb is not None:
                gdb.close()
            event("proxy_exit", outcome=outcome, admitted=peer is not None,
                  last_rx=last_rx, next_tx=next_tx,
                  unconfirmed_input=pending[0] if pending else None, **stats)


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
