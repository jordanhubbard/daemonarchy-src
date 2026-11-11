#!/usr/bin/env python3
"""Compile the complete production AON driver and captured DTB hog properties.

Host MMIO and ownership fault controls, not electrical or native acceptance.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    fixture = json.loads((ROOT / 'tools/daemonarchy/tests/fixtures/pi500-aon-hogs.json').read_text())
    child, = fixture['children']
    props = child['properties']
    assert set(props) == {'gpio-hog', 'gpios', 'output-high', 'line-name'}
    assert props['gpio-hog'] == props['output-high'] == []
    pin, flags = props['gpios']
    with tempfile.TemporaryDirectory(prefix='bcmstb-hog-') as tmp:
        header = Path(tmp) / 'fixture.h'
        binary = Path(tmp) / 'control'
        header.write_text(f'#define SELECTED_HOG_PIN {pin}U\n'
                          f'#define SELECTED_HOG_FLAGS {flags}U\n'
                          '#define SELECTED_HOG_LINE_NAME ' + json.dumps(props['line-name']) + '\n')
        cmd = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
               '-include', str(header),
               '-I' + str(ROOT / 'tools/daemonarchy/tests/bcmstb-gpio-stubs'),
               str(ROOT / 'tools/daemonarchy/tests/bcmstb-gpio.c'), '-o', str(binary)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        if 'clang' in Path(args.cc).name:
            suppression = '-Wno-gcc-install-dir-libstdcxx'
            probe = subprocess.run([args.cc, '-Werror', suppression, '-x', 'c',
                                    '-fsyntax-only', '-'], input='', text=True,
                                   capture_output=True)
            if probe.returncode == 0:
                cmd.insert(1, suppression)
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
