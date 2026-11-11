#!/usr/bin/env python3
"""Compile actual regulator lifetime/producer functions with ownership faults.

The source functions and allocation-bearing types are read from production, not
reimplemented here. Kernel services are bounded substitutes; this is not native
Newbus, live SYSCTL concurrency, or hardware acceptance.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def block(path, expression):
    source = (ROOT / path).read_text()
    match = re.search(expression, source, re.M)
    if match is None:
        raise ValueError(f'missing production declaration: {path}: {expression}')
    start = match.start()
    body = source.index('{', match.start())
    depth, end = 1, body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    if source[end:end + 1] == ';':
        end += 1
    return f'\n#line {source[:start].count(chr(10)) + 1} "{path}"\n' + source[start:end] + '\n'


def function(path, name):
    return block(path, r'^(?:static )?(?:struct regnode \*|int|void)\s+\n?' + name + r'\(')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cflag', action='append', default=[])
    args = parser.parse_args()
    framework = 'sys/dev/regulator/regulator.c'
    header = 'sys/dev/regulator/regulator.h'
    producer = 'sys/dev/gpio/gpioregulator.c'
    types = ''.join(block(header, '^struct ' + name + r'[ \t]*\{') for name in
                    ('regnode_std_param', 'regnode_init_def'))
    types += ''.join(block(framework, '^struct ' + name + r'[ \t]*\{') for name in
                     ('regnode', 'regulator'))
    types += ''.join(block(producer, '^struct ' + name + r'[ \t]*\{') for name in
                     ('gpioregulator_state', 'gpioregulator_init_def',
                      'gpioregulator_reg_sc', 'gpioregulator_softc'))
    types += '\n' + '\n'.join(line for line in (ROOT / header).read_text().splitlines()
                              if line.startswith('#define REGULATOR_FLAGS_')) + '\n'
    functions = ''.join(function(framework, name) for name in
                        ('regnode_find_by_name', 'regnode_publish_sysctls',
                         'regnode_create', 'regnode_destroy_unpublished', 'regnode_register'))
    functions += ''.join(function(producer, name) for name in
                         ('gpioregulator_regnode_init', 'gpioregulator_parse_delay',
                          'gpioregulator_release_def', 'gpioregulator_parse_fdt',
                          'gpioregulator_attach'))
    harness = (ROOT / 'tools/daemonarchy/tests/regnode-lifetime.c').read_text()
    harness = harness.replace('/* PRODUCTION_TYPES */', types)
    harness = harness.replace('/* PRODUCTION_FUNCTIONS */', functions)
    with tempfile.TemporaryDirectory(prefix='regnode-lifetime-') as temporary:
        source = Path(temporary) / 'test.c'
        binary = Path(temporary) / 'test'
        source.write_text(harness)
        subprocess.run([args.cc, '-std=c17', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        *args.cflag, str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
