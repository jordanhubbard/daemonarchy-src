#!/usr/bin/env python3
"""Exercise the production diagnostic console with real UDP and subprocess IO."""
import json
import pathlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

SCRIPT = pathlib.Path(__file__).with_name('debugnet-console.py')
HDR = struct.Struct('!IIQII')
ACK = struct.Struct('!I')


class ConsoleTest(unittest.TestCase):
    def test_bidirectional_mountroot_wire_and_capture(self):
        with tempfile.TemporaryDirectory() as tmp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            capture = pathlib.Path(tmp) / 'capture.bin'
            proc = subprocess.Popen([sys.executable, str(SCRIPT), '--bind', '127.0.0.1',
                '--boot-id', '0123456789abcdef', '--timeout', '5', '--capture', str(capture)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                sock.bind(('127.0.0.1', 0))
                sock.settimeout(2)
                dest = ('127.0.0.1', 26003)
                time.sleep(.1)
                sock.sendto(HDR.pack(1, 1, 0, 16, 0) + b'wrong-boot-id!!', dest)
                with self.assertRaises(socket.timeout):
                    sock.settimeout(.15)
                    sock.recvfrom(1024)
                sock.settimeout(2)
                sock.sendto(HDR.pack(1, 1, 0, 16, 0) + b'0123456789abcdef', dest)
                self.assertEqual(sock.recvfrom(1024)[0], ACK.pack(1))
                proc.stdin.write(b'ufs:/dev/ada0p2\n')
                proc.stdin.flush()
                wire, _ = sock.recvfrom(1024)
                self.assertEqual(HDR.unpack_from(wire)[:2], (3, 1))
                self.assertEqual(wire[HDR.size:], ACK.pack(1) + b'ufs:/dev/ada0p2\n')
                # A lost ACK must retransmit the same input, not advance it.
                retransmit, _ = sock.recvfrom(1024)
                self.assertEqual(retransmit, wire)
                sock.sendto(ACK.pack(1), dest)
                output = b'\nmountroot> '
                sock.sendto(HDR.pack(3, 2, 0, len(output), 0) + output, dest)
                self.assertEqual(sock.recvfrom(1024)[0], ACK.pack(2))
                sock.sendto(HDR.pack(2, 3, 0, 0, 0), dest)
                self.assertEqual(sock.recvfrom(1024)[0], ACK.pack(3))
                proc.stdin.close()
                proc.wait(timeout=2)
                self.assertEqual(proc.returncode, 0, proc.stderr.read())
                self.assertEqual(capture.read_bytes(), output)
                self.assertEqual(proc.stdout.read(), output)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                if proc.stdin is not None and not proc.stdin.closed:
                    proc.stdin.close()
                proc.stdout.close()
                proc.stderr.close()


BOOT = b'0123456789abcdef'
DEST = ('127.0.0.1', 26003)


class FaultTest(unittest.TestCase):
    """D7 controller faults over real UDP sockets and a subprocess controller."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.capture = self.dir / 'capture.bin'
        self.state = self.dir / 'state.json'
        self.kernel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.kernel.bind(('127.0.0.1', 0))
        self.kernel.settimeout(2)
        self.procs = []

    def tearDown(self):
        for proc in self.procs:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                if stream is not None and not stream.closed:
                    stream.close()
        self.kernel.close()
        self.tmp.cleanup()

    def start(self, events, timeout=5, resume=False):
        args = [sys.executable, str(SCRIPT), '--bind', '127.0.0.1', '--boot-id',
                BOOT.decode(), '--timeout', str(timeout), '--capture', str(self.capture),
                '--events', str(self.dir / events), '--state', str(self.state)]
        if resume:
            args.append('--resume')
        proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        self.procs.append(proc)
        time.sleep(.15)
        return proc

    def send(self, kind, seq, payload=b''):
        self.kernel.sendto(HDR.pack(kind, seq, 0, len(payload), 0) + payload, DEST)

    def expect_ack(self, seq):
        self.assertEqual(self.kernel.recvfrom(1024)[0], ACK.pack(seq))

    def expect_silence(self):
        self.kernel.settimeout(.2)
        try:
            with self.assertRaises(socket.timeout):
                self.kernel.recvfrom(1024)
        finally:
            self.kernel.settimeout(2)

    def admit(self):
        self.send(1, 1, BOOT)
        self.expect_ack(1)

    def input_packet(self):
        wire, _ = self.kernel.recvfrom(1024)
        kind, seq, _, length, _ = HDR.unpack_from(wire)
        self.assertEqual(kind, 3)
        self.assertEqual(length, len(wire) - HDR.size)
        inner, = ACK.unpack_from(wire, HDR.size)
        self.assertEqual(inner, seq)
        return seq, wire[HDR.size + ACK.size:]

    def finish(self, proc, seq):
        self.send(2, seq)
        self.expect_ack(seq)
        proc.stdin.close()
        proc.wait(timeout=3)
        return proc.returncode

    def events(self, name):
        return [json.loads(line) for line in (self.dir / name).read_text().splitlines()]

    def test_forced_mountroot_transcript_from_input_file(self):
        """The native D4 job redirects one input file; replay its exact exchange."""
        source = self.dir / 'controller-input.txt'
        source.write_bytes(b'?\nufs:/dev/vtbd0p2\n')
        with source.open('rb') as stdin:
            proc = subprocess.Popen([sys.executable, str(SCRIPT), '--bind', '127.0.0.1',
                '--boot-id', BOOT.decode(), '--timeout', '5', '--capture', str(self.capture),
                '--events', str(self.dir / 'events.jsonl')], stdin=stdin,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(proc)
        time.sleep(.15)
        self.admit()
        # Input is sent once admitted; the kernel ring holds it for mountroot.
        self.assertEqual(self.input_packet(), (1, b'?\n'))
        self.kernel.sendto(ACK.pack(1), DEST)
        self.assertEqual(self.input_packet(), (2, b'ufs:/dev/vtbd0p2\n'))
        self.kernel.sendto(ACK.pack(2), DEST)
        prompt = b'\nmountroot> '
        self.send(3, 2, prompt)
        self.expect_ack(2)
        listing = b'?\n\nList of GEOM managed disk devices:\n  vtbd0p2 vtbd0p1 vtbd0\n\nmountroot> '
        self.send(3, 3, listing)
        self.expect_ack(3)
        self.send(2, 4)
        self.expect_ack(4)
        proc.wait(timeout=3)
        self.assertEqual(proc.returncode, 0, proc.stderr.read())
        self.assertEqual(self.capture.read_bytes(), prompt + listing)
        names = [r['event'] for r in self.events('events.jsonl')]
        self.assertEqual(names, ['kernel_connected', 'input_sent', 'input_acked',
                                 'input_sent', 'input_acked', 'controller_exit'])

    def test_malformed_and_foreign_packets_are_counted_and_ignored(self):
        proc = self.start('events.jsonl')
        self.kernel.sendto(b'short', DEST)
        self.kernel.sendto(HDR.pack(1, 1, 0, 99, 0) + BOOT, DEST)
        self.kernel.sendto(ACK.pack(1), DEST)
        self.send(1, 1, b'fedcba9876543210')
        self.expect_silence()
        self.admit()
        self.kernel.sendto(HDR.pack(3, 2, 0, 1, 0) + b'too long', DEST)
        self.send(9, 2, b'unknown kind')
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as foreign:
            foreign.sendto(HDR.pack(3, 2, 0, 5, 0) + b'spoof', DEST)
        self.expect_silence()
        self.send(3, 2, b'mountroot> ')
        self.expect_ack(2)
        self.assertEqual(self.finish(proc, 3), 0, proc.stderr.read())
        self.assertEqual(self.capture.read_bytes(), b'mountroot> ')
        summary = self.events('events.jsonl')[-1]
        self.assertEqual(summary['event'], 'controller_exit')
        self.assertEqual(summary['outcome'], 'finished')
        self.assertEqual(summary['malformed'], 4)
        self.assertEqual(summary['unexpected'], 2)
        self.assertEqual(summary['foreign_peer'], 1)
        self.assertEqual(summary['output_bytes'], 11)

    def test_buffered_pipe_input_is_split_into_bounded_ordered_chunks(self):
        proc = self.start('events.jsonl')
        self.admit()
        proc.stdin.write(b'?\n' + b'x' * 600 + b'\nufs:/dev/vtbd0p2\n')
        proc.stdin.flush()
        chunks = []
        for expected in range(1, 6):
            seq, chunk = self.input_packet()
            self.assertEqual(seq, expected)
            self.assertLessEqual(len(chunk), 255)
            chunks.append(chunk)
            self.kernel.sendto(ACK.pack(seq), DEST)
        self.assertEqual(chunks, [b'?\n', b'x' * 255, b'x' * 255, b'x' * 90 + b'\n',
                                  b'ufs:/dev/vtbd0p2\n'])
        self.assertEqual(self.finish(proc, 2), 0, proc.stderr.read())

    def test_kernel_send_timeout_reuses_sequence_and_gap_is_recorded(self):
        proc = self.start('events.jsonl')
        self.admit()
        self.send(3, 2, b'a')
        self.expect_ack(2)
        self.send(3, 2, b'a')  # lost ACK: retransmitted, already captured
        self.expect_ack(2)
        self.send(3, 2, b'b')  # kernel timed out and reused sequence 2
        self.expect_ack(2)
        self.send(3, 4, b'c')  # a sequence gap is preserved, not hidden
        self.expect_ack(4)
        self.assertEqual(self.finish(proc, 5), 0, proc.stderr.read())
        self.assertEqual(self.capture.read_bytes(), b'abc')
        summary = self.events('events.jsonl')[-1]
        self.assertEqual((summary['duplicate_output'], summary['sequence_reuse'],
                          summary['sequence_gap']), (1, 1, 1))

    def test_unacknowledged_input_fails_with_preserved_record(self):
        proc = self.start('events.jsonl', timeout=1)
        self.admit()
        self.send(3, 2, b'mountroot> ')
        self.expect_ack(2)
        proc.stdin.write(b'?\n')
        proc.stdin.flush()
        first = self.kernel.recvfrom(1024)[0]
        retries = 0
        self.kernel.settimeout(1.5)
        while True:
            try:
                wire = self.kernel.recvfrom(1024)[0]
            except socket.timeout:
                break
            self.assertEqual(wire, first)
            retries += 1
        proc.wait(timeout=3)
        self.assertEqual(proc.returncode, 1)
        self.assertIn(b'did not acknowledge', proc.stderr.read())
        self.assertEqual(retries, 5)
        self.assertEqual(self.capture.read_bytes(), b'mountroot> ')
        records = self.events('events.jsonl')
        self.assertIn('input_unacknowledged', [r['event'] for r in records])
        self.assertEqual(records[-1]['outcome'], 'error')
        self.assertEqual(records[-1]['unconfirmed_input'], 1)
        self.assertEqual(records[-1]['input_retransmit'], 5)

    def test_missing_kernel_idle_timeout_is_recorded(self):
        proc = self.start('events.jsonl', timeout=.5)
        proc.wait(timeout=3)
        self.assertEqual(proc.returncode, 1)
        summary = self.events('events.jsonl')[-1]
        self.assertEqual((summary['outcome'], summary['admitted']), ('idle_timeout', False))
        self.assertFalse(self.state.exists())

    def test_reherald_resets_kernel_output_sequence(self):
        proc = self.start('events.jsonl')
        self.admit()
        self.send(3, 2, b'one')
        self.expect_ack(2)
        self.admit()
        self.send(3, 2, b'two')
        self.expect_ack(2)
        self.assertEqual(self.finish(proc, 3), 0, proc.stderr.read())
        self.assertEqual(self.capture.read_bytes(), b'onetwo')
        self.assertEqual(self.events('events.jsonl')[-1]['reherald'], 1)

    def test_controller_restart_resumes_admitted_session(self):
        first = self.start('events-1.jsonl')
        self.admit()
        self.send(3, 2, b'mountroot> ')
        self.expect_ack(2)
        first.stdin.write(b'?\n')
        first.stdin.flush()
        self.assertEqual(self.input_packet(), (1, b'?\n'))
        self.kernel.sendto(ACK.pack(1), DEST)
        first.stdin.write(b'lost\n')
        first.stdin.flush()
        self.assertEqual(self.input_packet(), (2, b'lost\n'))
        first.send_signal(signal.SIGKILL)
        first.wait(timeout=3)
        state = json.loads(self.state.read_text())
        self.assertEqual((state['rx_last'], state['tx_next'], state['unconfirmed_input']),
                         (2, 3, 2))

        second = self.start('events-2.jsonl', resume=True)
        self.send(3, 2, b'mountroot> ')  # retransmission across the restart
        self.expect_ack(2)
        self.send(3, 3, b'\nList of GEOM managed disk devices:')
        self.expect_ack(3)
        second.stdin.write(b'ufs:/dev/vtbd0p2\n')
        second.stdin.flush()
        # The kernel discards input sequences it has seen; never reuse one.
        self.assertEqual(self.input_packet(), (3, b'ufs:/dev/vtbd0p2\n'))
        self.kernel.sendto(ACK.pack(3), DEST)
        self.assertEqual(self.finish(second, 4), 0, second.stderr.read())
        self.assertEqual(self.capture.read_bytes(),
                         b'mountroot> \nList of GEOM managed disk devices:')
        records = self.events('events-2.jsonl')
        self.assertEqual(records[0]['event'], 'controller_resumed')
        self.assertEqual(records[0]['unconfirmed_input'], 2)
        self.assertEqual(records[-1]['duplicate_output'], 1)

    def test_resume_refuses_mismatched_or_missing_state(self):
        self.state.write_text(json.dumps({'schema': 'daemonarchy-debugnet-console-state@1',
                                          'boot_id': 'another-boot', 'peer': ['127.0.0.1', 1],
                                          'rx_last': 1, 'tx_next': 1}))
        proc = self.start('events-1.jsonl', resume=True)
        proc.wait(timeout=3)
        self.assertEqual(proc.returncode, 1)
        self.assertIn(b'does not match', proc.stderr.read())
        self.state.unlink()
        proc = self.start('events-2.jsonl', resume=True)
        proc.wait(timeout=3)
        self.assertEqual(proc.returncode, 1)


if __name__ == '__main__':
    unittest.main()
