#!/usr/bin/env python3
"""Explicit plain debugnet console; expects the loader's boot ID."""
import argparse
import hashlib
import json
import os
import select
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from netdebug_stage import StageRecorder  # noqa: E402

HEADER = struct.Struct('!IIQII')
ACK = struct.Struct('!I')
HERALD, FINISHED, DATA = 1, 2, 3
PORT = 26003
# The kernel input ring holds DNC_BUFSIZE - 1 (511) bytes; a larger chunk is
# refused with ENOBUFS forever.  Keep each chunk well below that bound.
INPUT_CHUNK = 255
MAX_PENDING_INPUT = 65536
COUNTERS = ('malformed', 'foreign_peer', 'unexpected', 'duplicate_output',
            'sequence_reuse', 'sequence_gap', 'input_retransmit', 'stale_ack',
            'reherald', 'output_bytes', 'input_bytes')
STATE_SCHEMA = 'daemonarchy-debugnet-console-state@1'


def load_state(path, expected):
    with open(path, encoding='utf-8') as handle:
        state = json.load(handle)
    if state.get('schema') != STATE_SCHEMA or state.get('boot_id') != expected:
        raise ValueError('controller state does not match this boot ID')
    peer = state.get('peer')
    if (not isinstance(peer, list) or len(peer) != 2 or
            not isinstance(peer[0], str) or not isinstance(peer[1], int)):
        raise ValueError('controller state has no admitted kernel peer')
    for key in ('rx_last', 'tx_next'):
        if not isinstance(state.get(key), int) or not 0 <= state[key] < 2 ** 32:
            raise ValueError(f'controller state has invalid {key}')
    return state


def save_state(path, state):
    tmp = f'{path}.tmp'
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as handle:
        json.dump(state, handle, sort_keys=True)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, path)


def serve(bind, expected, timeout, capture, *, listener=None, on_admit=None,
          events=None, state_path=None, resume=False, stage=None, finish_stage=True):
    stats = dict.fromkeys(COUNTERS, 0)
    peer = None
    rx_last = 0
    rx_digest = None
    tx_seq = 1
    pending = None
    inbuf = bytearray()
    stdin_open = True
    outcome = 'error'

    def event(name, **fields):
        if events is not None:
            events.write(json.dumps({'event': name, 'time': time.time(), **fields},
                                    sort_keys=True) + '\n')
            events.flush()

    def persist():
        if state_path is not None and peer is not None:
            save_state(state_path, {
                'schema': STATE_SCHEMA, 'boot_id': expected, 'peer': list(peer),
                'rx_last': rx_last, 'rx_digest': rx_digest, 'tx_next': tx_seq,
                'unconfirmed_input': pending[0] if pending else None})

    if resume:
        state = load_state(state_path, expected)
        peer = (state['peer'][0], state['peer'][1])
        rx_last, rx_digest, tx_seq = state['rx_last'], state.get('rx_digest'), state['tx_next']
        event('controller_resumed', peer=peer[0], rx_last=rx_last, tx_next=tx_seq,
              unconfirmed_input=state.get('unconfirmed_input'))
        if on_admit is not None:
            on_admit(peer)

    with (listener or socket.socket(socket.AF_INET, socket.SOCK_DGRAM)) as sock:
        try:
            if listener is None:
                sock.bind((bind, PORT))
            sock.setblocking(False)
            deadline = time.monotonic() + timeout
            # Pending input is bounded by its own retransmission limit.
            while pending or time.monotonic() < deadline:
                now = time.monotonic()
                if pending and now >= pending[2]:
                    if pending[3] >= max(5, min(1200, int(timeout / .5))):
                        event('input_unacknowledged', sequence=pending[0])
                        raise TimeoutError('kernel did not acknowledge input')
                    sock.sendto(pending[1], peer)
                    stats['input_retransmit'] += 1
                    pending = (pending[0], pending[1], now + .5, pending[3] + 1)
                if peer and pending is None and inbuf:
                    end = inbuf.find(b'\n', 0, INPUT_CHUNK)
                    chunk = bytes(inbuf[:end + 1 if end >= 0 else INPUT_CHUNK])
                    del inbuf[:len(chunk)]
                    payload = ACK.pack(tx_seq) + chunk
                    wire = HEADER.pack(DATA, tx_seq, 0, len(payload), 0) + payload
                    pending = (tx_seq, wire, time.monotonic() + .5, 0)
                    tx_seq += 1
                    # Record the consumed sequence before the kernel can see it.
                    persist()
                    sock.sendto(wire, peer)
                    stats['input_bytes'] += len(chunk)
                    event('input_sent', sequence=pending[0], bytes=len(chunk))
                readers = [sock]
                if peer and stdin_open and len(inbuf) < MAX_PENDING_INPUT:
                    readers.append(sys.stdin)
                ready, _, _ = select.select(readers, [], [], .1)
                if sys.stdin in ready:
                    data = sys.stdin.buffer.read1(4096)
                    if data:
                        inbuf.extend(data)
                    else:
                        stdin_open = False
                if sock not in ready:
                    continue
                packet, addr = sock.recvfrom(2048)
                if peer and addr != peer:
                    stats['foreign_peer'] += 1
                    continue
                if len(packet) == ACK.size and peer:
                    seq, = ACK.unpack(packet)
                    if pending and seq == pending[0]:
                        pending = None
                        persist()
                        event('input_acked', sequence=seq)
                    else:
                        stats['stale_ack'] += 1
                    continue
                if len(packet) < HEADER.size:
                    stats['malformed'] += 1
                    continue
                kind, seq, _, length, _ = HEADER.unpack_from(packet)
                if length != len(packet) - HEADER.size:
                    stats['malformed'] += 1
                    continue
                payload = packet[HEADER.size:]
                if kind == HERALD and payload.decode('ascii', 'replace') == expected and (
                        peer is None or addr == peer):
                    # A herald on an admitted session is a fresh kernel pcb.
                    if peer is not None:
                        stats['reherald'] += 1
                    peer = addr
                    sock.sendto(ACK.pack(seq), addr)
                    rx_last, rx_digest = seq, None
                    deadline = time.monotonic() + timeout
                    persist()
                    if stage is not None:
                        stage.stage('kernel_admitted', boot_id=expected, transport='debugnet')
                    event('kernel_connected', peer=addr[0], sequence=seq,
                          reherald=stats['reherald'] > 0)
                    if stats['reherald'] == 0 and on_admit is not None:
                        on_admit(addr)
                    print(f'kernel debugnet session {expected} from {addr}', file=sys.stderr)
                    continue
                if peer is None:
                    stats['unexpected'] += 1
                    continue
                deadline = time.monotonic() + timeout
                if kind == FINISHED:
                    sock.sendto(ACK.pack(seq), addr)
                    outcome = 'finished'
                    if stage is not None:
                        stage.stage('kernel_transport_closed')
                    return 0
                if kind != DATA:
                    stats['unexpected'] += 1
                    continue
                digest = hashlib.sha256(payload).hexdigest()
                # debugnet does not advance its sequence after a timed-out send,
                # so different bytes under the last sequence are new output.
                fresh = seq > rx_last or (seq == rx_last and digest != rx_digest)
                if fresh:
                    if seq > rx_last + 1:
                        stats['sequence_gap'] += 1
                        event('output_sequence_gap', expected=rx_last + 1, sequence=seq)
                    elif seq == rx_last:
                        stats['sequence_reuse'] += 1
                        event('output_sequence_reuse', sequence=seq)
                    capture.write(payload)
                    capture.flush()
                    if stage is not None:
                        stage.output(payload)
                    sys.stdout.buffer.write(payload)
                    sys.stdout.buffer.flush()
                    stats['output_bytes'] += len(payload)
                    rx_last, rx_digest = seq, digest
                    persist()
                else:
                    stats['duplicate_output'] += 1
                sock.sendto(ACK.pack(seq), addr)
            outcome = 'idle_timeout'
            raise TimeoutError('kernel session idle or missing')
        finally:
            if stage is not None and finish_stage:
                stage.finish(outcome)
            event('controller_exit', outcome=outcome, admitted=peer is not None,
                  rx_last=rx_last, tx_next=tx_seq, queued_input_bytes=len(inbuf),
                  unconfirmed_input=pending[0] if pending else None, **stats)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bind', default='0.0.0.0')
    p.add_argument('--boot-id', required=True)
    p.add_argument('--timeout', type=float, default=600)
    p.add_argument('--capture', required=True)
    p.add_argument('--events', help='new JSONL session/fault record')
    p.add_argument('--state', help='private session state for --resume')
    p.add_argument('--resume', action='store_true',
                   help='continue an admitted session after a controller restart')
    p.add_argument('--last-stage', help='last received stage record, kept after silence')
    a = p.parse_args()
    if a.timeout <= 0:
        p.error('timeout must be positive')
    if a.resume and not a.state:
        p.error('--resume needs --state')
    try:
        with open(a.capture, 'ab', buffering=0) as capture:
            events = open(a.events, 'x', encoding='utf-8') if a.events else None
            try:
                stage = (StageRecorder(a.last_stage, a.boot_id, resume=a.resume)
                         if a.last_stage else None)
                return serve(a.bind, a.boot_id, a.timeout, capture, events=events,
                             state_path=a.state, resume=a.resume, stage=stage)
            finally:
                if events is not None:
                    events.close()
    except (OSError, TimeoutError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
