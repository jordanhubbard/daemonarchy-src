#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Execute selected original targets and report observed outcomes, never admission."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def run(argv, *, env=None):
    print(json.dumps({'argv': [str(v) for v in argv]}), flush=True)
    subprocess.run([str(v) for v in argv], env=env, stdin=subprocess.DEVNULL, check=True, timeout=180)


def case_names(content):
    lines = content.splitlines()
    if not lines or lines[0] != 'Content-Type: application/X-atf-tp; version="1"':
        raise ValueError('original program returned an invalid ATF listing')
    names = [line.removeprefix('ident: ') for line in lines if line.startswith('ident: ')]
    if not names or len(names) != len(set(names)):
        raise ValueError('original ATF listing is empty or duplicated')
    return set(names)


def require_case_listing(content, expected):
    if case_names(content) != set(expected):
        raise ValueError('original ATF case listing differs from the reviewed inventory')


def require_linker_listing(content, expected):
    jobs = [shlex.split(line.strip()) for line in content.splitlines()
            if line.lstrip().startswith('"')]
    linkers = [job[0] for job in jobs if job and '-cc1' not in job and '-cc1as' not in job]
    if linkers != [str(expected)]:
        raise ValueError('compiler did not select the exact captured linker')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('build', 'tests', 'docs', 'accept'))
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--objects', type=Path, required=True)
    parser.add_argument('--outputs', type=Path, required=True)
    args = parser.parse_args()
    inputs, objects, outputs = (p.resolve(strict=True) for p in (args.inputs, args.objects, args.outputs))
    seed = inputs / 'tools/daemonarchy/retained/bootstrap/tree'
    env = dict(os.environ)
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    env.update(ATF_SHELL=str(seed / 'bin/sh'), ATF_LIBEXECDIR=str(seed / 'usr/libexec'),
               ATF_PKGDATADIR=str(seed / 'usr/share/atf'))
    products = outputs / 'products'
    if args.stage == 'build':
        compiler = [seed / 'usr/bin/cc', '--sysroot=' + str(seed),
                    '-resource-dir', seed / 'usr/lib/clang/21', '-B' + str(seed / 'usr/bin')]
        linker_plan = subprocess.run([str(v) for v in [*compiler, '-###', '-x', 'c',
                                     os.devnull, '-o', objects / 'link-selection']],
                                    env=env, stdin=subprocess.DEVNULL, text=True,
                                    capture_output=True, check=True, timeout=30)
        print(linker_plan.stderr, end='', flush=True)
        require_linker_listing(linker_plan.stderr, seed / 'usr/bin/ld.lld')
        shell = objects / 'captured-shell.mk'
        shell.write_text('.SHELL: name=sh path=' + str(seed / 'bin/sh') + '\n')
        make = [seed / 'usr/bin/make', '-m', inputs / 'share/mk',
                'SRCTOP=' + str(inputs), '__MAKE_CONF=/dev/null', 'SRCCONF=/dev/null',
                'SRC_ENV_CONF=/dev/null', 'WITHOUT_MAN=yes', 'MACHINE=amd64',
                'MACHINE_ARCH=amd64', 'CC=' + str(seed / 'usr/bin/cc') +
                ' --sysroot=' + str(seed) + ' -resource-dir ' + str(seed / 'usr/lib/clang/21') +
                ' -B' + str(seed / 'usr/bin'),
                'LD=' + str(seed / 'usr/bin/ld.lld'),
                'AR=' + str(seed / 'usr/bin/ar'), 'RANLIB=' + str(seed / 'usr/bin/ranlib'),
                'NM=' + str(seed / 'usr/bin/nm'), 'OBJCOPY=' + str(seed / 'usr/bin/objcopy')]
        def build(name, source, options):
            directory = objects / name
            directory.mkdir()
            run([*make, '-C', inputs / source, '-f', shell, '-f', 'Makefile', *options, 'all'],
                env={**env, 'MAKEOBJDIR': str(directory)})
            return directory
        library = build('parser', 'lib/libutil', ['LIB=daemonarchy_expand_number',
                        'SRCS=expand_number.c', 'INCS=', 'INTERNALLIB=1', 'WITHOUT_TESTS=yes'])
        archive = library / 'libdaemonarchy_expand_number_pie.a'
        cflags = 'CFLAGS=-O2 -pipe -I' + str(inputs / 'lib/libutil')
        utility = build('utility', 'usr.bin/truncate', ['LIBADD=', 'LDADD=' + str(archive), cflags])
        parser_tests = build('parser-tests', 'lib/libutil/tests',
                            ['ATF_TESTS_C=expand_number_test', 'TAP_TESTS_C=', 'LIBADD=',
                             'LIBATF_C=' + str(seed / 'usr/lib/libprivateatf-c.a'),
                             'LDADD=' + str(archive) + ' -lprivateatf-c', cflags])
        shell_tests = build('utility-tests', 'usr.bin/truncate/tests', [])
        for relative, source in [('bin/truncate', utility / 'truncate'),
                                 ('lib/libdaemonarchy_expand_number_pie.a', archive),
                                 ('include/libutil.h', inputs / 'lib/libutil/libutil.h'),
                                 ('tests/expand_number_test', parser_tests / 'expand_number_test'),
                                 ('tests/truncate_test.body', shell_tests / 'truncate_test')]:
            destination = products / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        launcher = products / 'tests/truncate_test'
        variables = {'ATF_SHELL': str(seed / 'bin/sh'),
                     'ATF_LIBEXECDIR': str(seed / 'usr/libexec'),
                     'ATF_PKGDATADIR': str(seed / 'usr/share/atf'),
                     'PATH': ':'.join(str(p) for p in (products / 'bin', seed / 'bin',
                                     seed / 'usr/bin', seed / 'usr/libexec', seed / 'usr/local/bin'))}
        launcher.write_text('#!' + str(seed / 'bin/sh') + '\n' +
                            ''.join('export ' + k + '=' + shlex.quote(v) + '\n' for k, v in variables.items()) +
                            'exec ' + shlex.quote(str(seed / 'usr/libexec/atf-sh')) + ' ' +
                            shlex.quote(str(products / 'tests/truncate_test.body')) + ' "$@"\n')
        launcher.chmod(0o755)
    elif args.stage == 'tests':
        inventory = inputs / 'tools/daemonarchy/retained/original-cases.json'
        cases = json.loads(inventory.read_bytes())['cases']
        for program in sorted({c['program'] for c in cases}):
            actual = subprocess.check_output([str(products / 'tests' / program), '-l'],
                                             env=env, stdin=subprocess.DEVNULL, text=True, timeout=30)
            expected = {c['case'] for c in cases if c['program'] == program}
            require_case_listing(actual, expected)
        kyuafile = objects / 'Kyuafile'
        kyuafile.write_text('syntax(2)\ntest_suite("daemonarchy-original")\n' +
                           ''.join('atf_test_program{name="' + c + '"}\n' for c in sorted({v['program'] for v in cases})))
        database = objects / 'original.db'
        run([seed / 'usr/bin/kyua', '-c', 'none', 'test', '--kyuafile=' + str(kyuafile),
             '--build-root=' + str(products / 'tests'), '--results-file=' + str(database)], env=env)
        run([sys.executable, '-B', inputs / 'tools/daemonarchy/report-kyua.py', '--results', database,
             '--inventory', inventory, '--action', 'original-tests',
             '--output', outputs / 'original-tests.json',
             '--provenance', objects / 'original-provenance.json'], env=env)
    elif args.stage == 'docs':
        destination = outputs / 'documentation'
        destination.mkdir()
        for name in ('usr.bin/truncate/truncate.1', 'lib/libutil/expand_number.3'):
            source = inputs / name
            run([seed / 'usr/bin/mandoc', '-Tlint', source], env=env)
            shutil.copy2(source, destination / source.name)
    else:
        # The framework's withheld verifier runs after this preflight. This stage
        # does not construct a test report or award independent acceptance.
        if (products / 'bin/truncate').read_bytes()[:4] != b'\x7fELF':
            raise ValueError('sealed utility is not a native ELF product')
        if (products / 'lib/libdaemonarchy_expand_number_pie.a').read_bytes()[:8] != b'!<arch>\n':
            raise ValueError('sealed private library is not an archive')


if __name__ == '__main__':
    main()
