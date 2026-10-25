#!/usr/bin/env python3
"""Explicit plain debugnet console; expects the loader's boot ID."""
import argparse
import select
import socket
import struct
import sys
import time

HEADER = struct.Struct('!IIQII')
ACK = struct.Struct('!I')
HERALD, FINISHED, DATA = 1, 2, 3
PORT = 26003


def serve(bind, expected, timeout, capture, *, listener=None, on_admit=None):
    peer = None
    rx_last = 0
    tx_seq = 1
    pending = None
    stdin_open = True
    with (listener or socket.socket(socket.AF_INET, socket.SOCK_DGRAM)) as sock:
        if listener is None:
            sock.bind((bind, PORT))
        sock.setblocking(False)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if pending and time.monotonic() >= pending[2]:
                if pending[3] >= max(5, min(1200, int(timeout / .5))):
                    raise TimeoutError('kernel did not acknowledge input')
                sock.sendto(pending[1], peer)
                pending = (pending[0], pending[1], time.monotonic() + .5, pending[3] + 1)
            readers = [sock]
            if peer and pending is None and stdin_open:
                readers.append(sys.stdin)
            ready, _, _ = select.select(readers, [], [], .1)
            if sys.stdin in ready:
                line = sys.stdin.buffer.readline(512)
                if line:
                    payload = ACK.pack(tx_seq) + line
                    wire = HEADER.pack(DATA, tx_seq, 0, len(payload), 0) + payload
                    sock.sendto(wire, peer)
                    pending = (tx_seq, wire, time.monotonic() + .5, 0)
                else:
                    stdin_open = False
            if sock not in ready:
                continue
            packet, addr = sock.recvfrom(2048)
            if peer and addr != peer:
                continue
            if len(packet) == ACK.size and peer:
                seq, = ACK.unpack(packet)
                if pending and seq == pending[0]:
                    pending = None
                    tx_seq += 1
                continue
            if len(packet) < HEADER.size:
                continue
            kind, seq, _, length, _ = HEADER.unpack_from(packet)
            if length != len(packet) - HEADER.size:
                continue
            payload = packet[HEADER.size:]
            if peer is None:
                if kind != HERALD or payload.decode('ascii', 'replace') != expected:
                    continue
                peer = addr
                sock.sendto(ACK.pack(seq), addr)
                rx_last = seq
                deadline = time.monotonic() + timeout
                if on_admit is not None:
                    on_admit(addr)
                print(f'kernel debugnet session {expected} from {addr}', file=sys.stderr)
                continue
            deadline = time.monotonic() + timeout
            if kind == FINISHED:
                sock.sendto(ACK.pack(seq), addr)
                return 0
            if kind == DATA:
                if seq > rx_last:
                    capture.write(payload)
                    capture.flush()
                    sys.stdout.buffer.write(payload)
                    sys.stdout.buffer.flush()
                    rx_last = seq
                sock.sendto(ACK.pack(seq), addr)
        raise TimeoutError('kernel session idle or missing')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bind', default='0.0.0.0')
    p.add_argument('--boot-id', required=True)
    p.add_argument('--timeout', type=float, default=600)
    p.add_argument('--capture', required=True)
    a = p.parse_args()
    try:
        with open(a.capture, 'ab', buffering=0) as capture:
            return serve(a.bind, a.boot_id, a.timeout, capture)
    except (OSError, TimeoutError) as exc:
        print(exc, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
