#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Whole PL011 receive and shared UART filter/TTY host controls.

A finite DR source refills on reads, with no SWI execution inside the hard IRQ.
The original receive function is retained with its notices as a negative fixture.
This is not native UART, hardware flow-control, IRQ timing or lossless-delivery proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


def function(text, name):
    match = re.search(r'^' + re.escape(name) + r'\(', text, re.M)
    if match is None:
        raise ValueError('missing actual function: ' + name)
    start = text.rfind('\n', 0, match.start() - 1) + 1
    pos = text.index('{', match.end()) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def defines(text, pattern):
    return '\n'.join(line for line in text.splitlines()
                     if re.match(r'#define\s+' + pattern, line)) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix='pl011-rx-'))
    out.mkdir(parents=True, exist_ok=True)
    paths = {name: ROOT / rel for name, rel in {
        'pl011': 'sys/dev/uart/uart_dev_pl011.c', 'core': 'sys/dev/uart/uart_core.c',
        'tty': 'sys/dev/uart/uart_tty.c', 'bus': 'sys/dev/uart/uart_bus.h',
        'serial': 'sys/sys/serial.h', 'filter': 'sys/sys/bus.h',
        'termios': 'sys/sys/_termios.h', 'ttydisc': 'sys/sys/ttydisc.h'}.items()}
    text = {name: path.read_text() for name, path in paths.items()}
    baseline = HERE / 'fixtures/pl011-rx-before.c'
    fixture_path = HERE / 'fixtures/pl011-rx.json'
    fixture = json.loads(fixture_path.read_text())
    if sha(baseline) != fixture['baseline_fixture_sha256']:
        raise ValueError('retained original receive fixture changed')
    old = function(baseline.read_text(), 'uart_pl011_bus_receive').replace(
        'uart_pl011_bus_receive(', 'baseline_receive(', 1)
    notice = ''.join(text[name][:text[name].index('#include')] for name in ['pl011', 'core', 'tty'])
    const = text['pl011'][text['pl011'].index('/* PL011 UART registers'):text['pl011'].index('/*\n * The hardware FIFOs')]
    const += defines(text['bus'], r'UART_(?:STAT_|DRAIN_|FLUSH_)')
    const += defines(text['serial'], r'SER_')
    const += defines(text['filter'], r'FILTER_')
    const += defines(text['termios'], r'CRTS_IFLOW\b')
    const += defines(text['ttydisc'], r'TRE_')
    state = re.search(r'struct uart_pl011_softc \{.*?\n\};', text['pl011'], re.S)[0]
    rings = '\n'.join(function(text['bus'], n) for n in ['uart_rx_empty', 'uart_rx_full', 'uart_rx_get', 'uart_rx_next', 'uart_rx_peek', 'uart_rx_put'])
    driver = '\n'.join(function(text['pl011'], n) for n in ['uart_pl011_bus_receive', 'uart_pl011_bus_ipend', 'uart_pl011_bus_flush', 'uart_pl011_bus_setsig'])
    services = r'''
static uint32_t __uart_getreg(struct uart_bas *bas, unsigned reg) {
    (void)bas; assert(lock_depth == 1);
    if (reg == UART_FR) return input_pos < input_count ? FR_TXFE | (input_count-input_pos >= 16 ? FR_RXFF : 0) : FR_TXFE | FR_RXFE;
    if (reg == UART_DR) { assert(input_pos < input_count); data_reads++; return input[input_pos++]; }
    if (reg == UART_MIS) {
        mis_reads++;
        if (bounded_old && mis_reads > 16) longjmp(witness, 1);
        assert(mis_reads < 64);
        return input_pos < input_count ? irq_kind & live_imsc : 0;
    }
    assert(0); return 0;
}
static int receive_dispatch(struct uart_softc *sc) {
    return use_baseline ? baseline_receive(sc) : uart_pl011_bus_receive(sc);
}
'''
    core = '\n'.join(function(text['core'], n) for n in ['uart_sched_softih', 'uart_intr_break', 'uart_intr_overrun', 'uart_intr_rxready', 'uart_intr_txidle', 'uart_intr'])
    tty = '\n'.join(function(text['tty'], n) for n in ['uart_tty_inwakeup', 'uart_tty_intr'])
    vectors = 'struct vector { unsigned size,get,put,queued,total,expected_put,discard; };\nstatic const struct vector vectors[] = {\n'
    for row in fixture['vectors']:
        vectors += '{' + ','.join(str(row[key]) for key in ['ring_size', 'initial_get', 'initial_put', 'initial_queued', 'finite_input_bytes', 'expected_put', 'expected_discarded']) + '},\n'
    vectors += '};\n'
    unit = notice + (HERE/'pl011-rx-prefix.h').read_text() + const + state + rings + driver + old + services + core + tty + vectors + (HERE/'pl011-rx.c').read_text()
    (out/'unit.c').write_text(unit)
    command = [args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
               '-Wno-cast-function-type', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
               str(out/'unit.c'), '-o', str(out/'test')]
    if 'clang' in Path(args.cc).name:
        flag = '-Wno-gcc-install-dir-libstdcxx'
        probe = subprocess.run([args.cc, '-Werror', flag, '-x', 'c', '-fsyntax-only', '-'], input='', text=True, capture_output=True)
        if probe.returncode == 0:
            command.insert(1, flag)
    compiled = subprocess.run(command, text=True, capture_output=True)
    (out/'compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr, end='')
    compiled.check_returncode()
    run = subprocess.run([str(out/'test')], text=True, capture_output=True)
    (out/'test.log').write_text(run.stdout + run.stderr)
    if run.returncode:
        print(run.stdout + run.stderr, end='')
    run.check_returncode()
    if not re.search(r'^PASS: \d+ whole PL011 receive/core IRQ/ring/TTY host checks;', run.stdout, re.M):
        raise ValueError('missing case completion')
    report = {'ok': True, 'scope': __doc__, 'source_sha256': {str(p.relative_to(ROOT)): sha(p) for p in paths.values()},
              'compiler_command': command, 'stdout': run.stdout, 'baseline_fixture_sha256': sha(baseline),
              'fixture_sha256': sha(fixture_path), 'generated_source_sha256': sha(out/'unit.c'),
              'authored_test_sha256': {n: sha(HERE/n) for n in ['pl011-rx-controls.py', 'pl011-rx-prefix.h', 'pl011-rx.c']}}
    (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(run.stdout, end='')
    print(out)


if __name__ == '__main__':
    main()
