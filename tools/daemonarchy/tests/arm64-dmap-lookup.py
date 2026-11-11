#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Host controls for whole production DMAP bootstrap and lookup functions.

Typed mocks replace MMU/boot services. PROT_NONE memory witnesses unsafe table
access; this is not a native kernel, MMU, timing, permission or I/O qualification.
An optional --baseline-source exercises an independently retained old function.
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
        raise ValueError('missing production function: ' + name)
    start = text.rfind('\n', 0, match.start() - 1) + 1
    body = text.index('{', match.end())
    end, depth = body + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--source', type=Path, default=ROOT / 'sys/arm64/arm64/pmap.c')
    parser.add_argument('--baseline-source', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix='arm64-dmap-'))
    out.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text()
    lookup = function(source, 'pmap_klookup')
    bootstrap = function(source, 'pmap_bootstrap_dmap')
    if 'NO_PERTHREAD_SSP' not in lookup.split('{', 1)[0]:
        raise ValueError('lookup lost its per-thread stack protector exclusion')
    globals_start = source.index('#define\tPHYSMAP_SIZE')
    globals_end = source.index('static SYSCTL_NODE', globals_start)
    state = re.search(r'struct pmap_bootstrap_state \{.*?\n\};', source, re.S)[0]
    initializer = re.search(r'static struct pmap_bootstrap_state bs_state = \{.*?\n\};', source, re.S)[0]
    baseline = ''
    if args.baseline_source:
        baseline = function(args.baseline_source.read_text(), 'pmap_klookup')
        if 'dmap_mapped' in baseline:
            raise ValueError('baseline already has mapped-interval lookup')
        baseline = baseline.replace('pmap_klookup(', 'baseline_pmap_klookup(', 1)
    mocks = r'''
static void pmap_bootstrap_l0_table(struct pmap_bootstrap_state *s) {
    s->l1 = boot_l1;
}
static void advance(struct pmap_bootstrap_state *s, uint64_t end) {
    s->va += end - s->pa; s->pa = end;
}
static void pmap_bootstrap_l2_block(struct pmap_bootstrap_state *s, int i) {
    uint64_t end = rounddown2(physmap[i + 1], L2_SIZE);
    uint64_t boundary = roundup2(s->pa + 1, L1_SIZE);
    if (end > boundary) end = boundary;
    if (end > s->pa) advance(s, end);
}
static void pmap_bootstrap_l3_page(struct pmap_bootstrap_state *s, int i) {
    uint64_t end = rounddown2(physmap[i + 1], PAGE_SIZE);
    uint64_t boundary = roundup2(s->pa + 1, L2_SIZE);
    if (end > boundary) end = boundary;
    if (end > s->pa) advance(s, end);
}
static void pmap_s1_invalidate_all_kernel(void) {
    tlbi_calls++; tlbi_ready = bs_state.dmap_valid;
    assert(dmap_mapped_idx == supplied_count);
    for (unsigned i = 0; i < supplied_count; i += 2) {
        assert(dmap_mapped[i] == rounddown2(supplied[i], PAGE_SIZE));
        assert(dmap_mapped[i + 1] == supplied[i + 1]);
    }
}
static void physmem_exclude_region(vm_paddr_t pa, vm_size_t size, uint32_t flags) {
    (void)pa; (void)size; assert(flags == EXFLAG_NOALLOC);
    excludes++; exclude_ready = bs_state.dmap_valid;
}
'''
    fixture_path = HERE / 'fixtures/arm64-dmap-lookup.json'
    fixture = json.loads(fixture_path.read_text())
    header = 'static const vm_paddr_t fixture_ranges[] = {' + ','.join(
        'UINT64_C(' + hex(x) + ')' for pair in fixture['mapped_intervals'] for x in pair) + '};\n'
    header += 'static const struct { vm_offset_t va; vm_paddr_t pa; bool valid; } fixture_cases[] = {\n'
    for case in fixture['cases']:
        header += '{UINT64_C(' + case['virtual'] + '),UINT64_C(' + (case['expected_pa'] or '0') + '),' + str(case['expected_valid']).lower() + '},\n'
    header += '};\n'
    # Keep the source's original notices with the generated verbatim functions.
    notice = source[:source.index('#include')]
    unit = notice + (HERE / 'arm64-dmap-lookup-prefix.h').read_text() + source[globals_start:globals_end] + state + initializer + mocks + header + lookup + '\n' + baseline + '\n' + bootstrap + '\n#define HAVE_BASELINE ' + str(int(bool(baseline))) + '\n' + (HERE / 'arm64-dmap-lookup.c').read_text()
    (out / 'unit.c').write_text(unit)
    command = [args.cc, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-sign-compare',
               '-fstack-protector-strong', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', str(out / 'unit.c'), '-o', str(out / 'test')]
    if 'clang' in Path(args.cc).name:
        flag = '-Wno-gcc-install-dir-libstdcxx'
        probe = subprocess.run([args.cc, '-Werror', flag, '-x', 'c', '-fsyntax-only', '-'],
                               input='', text=True, capture_output=True)
        if probe.returncode == 0:
            command.insert(1, flag)
    compile_run = subprocess.run(command, text=True, capture_output=True)
    (out / 'compile.log').write_text(compile_run.stdout + compile_run.stderr)
    compile_run.check_returncode()
    run = subprocess.run([str(out / 'test')], text=True, capture_output=True)
    (out / 'test.log').write_text(run.stdout + run.stderr)
    run.check_returncode()
    if not re.search(r'^PASS: \d+ actual pmap lookup/bootstrap host controls;', run.stdout, re.M):
        raise ValueError('missing actual control completion')
    report = {'ok': True, 'scope': __doc__, 'source_sha256': digest(args.source),
              'fixture_sha256': digest(fixture_path), 'compiler_command': command,
              'controls_stdout': run.stdout, 'generated_source_sha256': digest(out / 'unit.c'),
              'baseline_source_sha256': digest(args.baseline_source) if args.baseline_source else None,
              'test_sources': {name: digest(HERE / name) for name in
                              ['arm64-dmap-lookup.py', 'arm64-dmap-lookup.c', 'arm64-dmap-lookup-prefix.h']}}
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(run.stdout, end='')
    print(out)


if __name__ == '__main__':
    main()
