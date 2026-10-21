#!/usr/bin/env python3
"""Destructive-to-test-variables check against an owned disposable UEFI loader.

Requires the matching loader-console.py and an explicitly provisioned private key.
It changes ldnc_count; never point it at production hardware.
"""
import argparse
import importlib.util
import json
import io
from pathlib import Path
import time

path = Path(__file__).resolve().parents[1] / 'loader-console.py'
spec = importlib.util.spec_from_file_location('loader_console', path)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def raw_receive(client, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if not m.select.select([client.socket], [], [], max(0, end-time.monotonic()))[0]:
            break
        data = client.socket.recv(2048)
        value = m.decode(client.key, data, client.session)
        if value:
            return data, value
    return None, None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('target')
    p.add_argument('--port', type=int, default=6668)
    p.add_argument('--key-file', type=Path, required=True)
    args = p.parse_args()
    key = m.read_key(args.key_file)
    output = io.BytesIO()
    c = m.Console(args.target, args.port, key, timeout=8, output=output)
    c.pair(); c.command('set ldnc_count=0'); c.close()
    c = m.Console(args.target, args.port, key, timeout=8, output=output)
    checks = []
    try:
        c.pair()
        boot, old_session = c.boot, c.session
        increment = b'loader.setenv("ldnc_count",tostring(tonumber(loader.getenv("ldnc_count") or "0")+1))\n'
        legitimate = m.encode(key, c.session, m.INPUT, 1, increment)
        corrupt = bytearray(legitimate); corrupt[-1] ^= 1
        wrong_key = m.encode(bytes(b ^ 1 for b in key), c.session, m.INPUT, 1, increment)
        gap = m.encode(key, c.session, m.INPUT, 2, increment)
        oversized = m.encode(key, c.session, m.INPUT, 1, b'x'*511+b'\n')
        for packet in (bytes(corrupt), wrong_key, legitimate[:-1], legitimate+b'x', gap, oversized):
            c.socket.send(packet)
        # Withhold OUTPUT_ACK; an INPUT_ACK reflected to the loader must not
        # settle output even when both directions happen to use sequence 1.
        c.socket.send(legitimate)
        first_output = reflected = None
        end = time.monotonic()+8
        while time.monotonic()<end and (first_output is None or reflected is None):
            data, value = raw_receive(c, .25)
            if not value: continue
            if value[0] == m.OUTPUT and value[1] == 1: first_output = data
            if value[0] == m.INPUT_ACK and value[1] == 1:
                reflected = data; c.socket.send(data)
        assert first_output is not None and reflected is not None, 'missing initial output/input admission'
        retransmitted = False
        end = time.monotonic()+4
        while time.monotonic()<end:
            data, value = raw_receive(c, .25)
            if value and value[0] == m.OUTPUT:
                assert value[1] == 1, 'reflected INPUT_ACK settled output'
                assert data == first_output, 'retransmission changed bytes'
                retransmitted = True; break
        assert retransmitted, 'unacknowledged output was not retransmitted'
        c.socket.send(m.encode(key, c.session, m.OUTPUT_ACK, 1))
        c.output_sequence = 2; c.last_output = first_output
        checks.append('output_ack_loss_and_input_ack_reflection')
        c.socket.send(legitimate)  # Exact retry is ACKed but not executed twice.
        changed = m.encode(key, c.session, m.INPUT, 1, b'set ldnc_count=99\n')
        c.socket.send(changed)
        c.input_sequence = 2
        output.seek(0); output.truncate()
        c.command('show ldnc_count')
        assert b'1\r\n' in output.getvalue() and b'99\r\n' not in output.getvalue(), 'invalid/retried input changed state'
        checks.append('forged_truncated_oversized_gap_duplicate_changed_input')
        c.close()
        c = m.Console(args.target, args.port, key, timeout=8, output=output)
        c.pair()
        assert c.boot == boot and c.session != old_session, 'pair did not rotate session within same boot'
        c.socket.send(legitimate)
        output.seek(0); output.truncate()
        c.command('show ldnc_count')
        assert b'1\r\n' in output.getvalue(), 'old-session input executed'
        checks.append('fresh_pairing_stable_boot_id_old_session_revoked')
        print(json.dumps({'checks': checks, 'passed': True, 'kernel_boot_observed': False}))
    finally:
        c.close()


if __name__ == '__main__':
    main()
