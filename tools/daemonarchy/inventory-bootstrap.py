#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Inventory explicit installed bootstrap inputs; never copy or grant admission."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import time

MAX_ENTRIES = 10000
MAX_FILE = 128 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024
ENVIRONMENT = {'PATH': '/bin:/usr/bin:/usr/local/bin', 'LC_ALL': 'C', 'TZ': 'UTC'}


def canonical_path(value):
    if (not isinstance(value, str) or not value.startswith('/') or value == '/' or
            any(part in ('', '.', '..') for part in value[1:].split('/')) or
            any(ord(c) < 32 or ord(c) == 127 for c in value) or '\\' in value):
        raise ValueError('bootstrap paths must be canonical absolute paths')
    return Path(value)


def stamp(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def unique_object(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError('duplicate selection field')
        result[key] = value
    return result


def inventory(selection, *, ldd='/usr/bin/ldd', timeout=120):
    if (not isinstance(selection, dict) or set(selection) !=
            {'schema', 'files', 'trees', 'library_roots'} or
            selection['schema'] != 'daemonarchy/bootstrap-toolchain-selection@1'):
        raise ValueError('invalid bootstrap selection')
    for key in ('files', 'trees', 'library_roots'):
        if not isinstance(selection[key], list) or len(selection[key]) > MAX_ENTRIES:
            raise ValueError('invalid selection sequence')
    libraries = tuple(canonical_path(p) for p in selection['library_roots'])
    selected_files = {canonical_path(p) for p in selection['files']}
    selected_trees = tuple(canonical_path(t['path']) for t in selection['trees']
                           if isinstance(t, dict) and 'path' in t)
    def permitted_reference(path):
        if path not in selected_files and not any(
                path.is_relative_to(root) for root in (*libraries, *selected_trees)):
            raise ValueError('bootstrap reference escaped selected roots: ' + str(path))

    entries, stamps, edges, pending, scripts = {}, {}, [], [], {}
    total = 0
    visiting = set()
    deadline = time.monotonic() + timeout

    def add(path):
        nonlocal total
        path = canonical_path(str(path))
        name = str(path)
        if name in visiting:
            raise ValueError('bootstrap linker script cycle')
        if name in entries:
            return
        if len(entries) >= MAX_ENTRIES or time.monotonic() >= deadline:
            raise ValueError('bootstrap inventory entry or time budget exceeded')
        before = path.lstat()
        mode = stat.S_IMODE(before.st_mode)
        item = {'mode': mode}
        if stat.S_ISDIR(before.st_mode):
            if path.resolve(strict=True) != path:
                raise ValueError('redirected directory ancestor')
            item['kind'] = 'directory'
        elif stat.S_ISLNK(before.st_mode):
            target = os.readlink(path)
            if target.startswith('/'):
                raise ValueError('absolute bootstrap symlink')
            destination = Path(os.path.normpath(str(path.parent / target)))
            permitted_reference(destination)
            item.update(kind='symlink', target=target)
            entries[name] = item
            stamps[name] = stamp(before)
            add(destination)
            if entries[str(destination)]['kind'] not in ('file', 'directory'):
                raise ValueError('chained bootstrap symlink requires separate review')
            return
        elif stat.S_ISREG(before.st_mode):
            if path.resolve(strict=True) != path or before.st_size > MAX_FILE:
                raise ValueError('redirected file or file byte budget exceeded')
            digest = hashlib.sha256()
            prefix = bytearray()
            count = 0
            with path.open('rb') as stream:
                if stamp(os.fstat(stream.fileno())) != stamp(before):
                    raise ValueError('bootstrap file replaced before read')
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    count += len(block)
                    if count > MAX_FILE:
                        raise ValueError('bootstrap file grew beyond byte budget')
                    digest.update(block)
                    if len(prefix) < 4096:
                        prefix.extend(block[:4096-len(prefix)])
                if stamp(os.fstat(stream.fileno())) != stamp(before):
                    raise ValueError('bootstrap file changed during read')
            if count != before.st_size or stamp(path.lstat()) != stamp(before):
                raise ValueError('bootstrap file changed during read')
            total += count
            if total > MAX_TOTAL:
                raise ValueError('bootstrap aggregate byte budget exceeded')
            item.update(kind='file', size=count, sha256=digest.hexdigest())
            if prefix[:4] == b'\x7fELF' and len(prefix) >= 18:
                byteorder = 'little' if prefix[5] == 1 else 'big'
                if int.from_bytes(prefix[16:18], byteorder) in (2, 3):
                    pending.append(path)
            elif prefix.lstrip().startswith(b'GROUP'):
                match = re.fullmatch(rb'\s*GROUP\s*\(\s*((?:/[A-Za-z0-9_./+-]+\s*)+)\)\s*', prefix)
                if not match or count > 4096:
                    raise ValueError('unsupported bootstrap linker script')
                references = match.group(1).decode('ascii').split()
                scripts[name] = {'sha256': item['sha256'], 'references': references}
                visiting.add(name)
                for reference in references:
                    dependency = canonical_path(reference)
                    permitted_reference(dependency)
                    add(dependency)
                visiting.remove(name)
        else:
            raise ValueError('special bootstrap input is not supported')
        entries[name] = item
        stamps[name] = stamp(before)

    def walk(path, excluded):
        add(path)
        if entries[str(path)]['kind'] == 'directory':
            for child in sorted(path.iterdir()):
                if child.name not in excluded:
                    walk(child, excluded)

    for value in selection['files']:
        add(canonical_path(value))
    for tree in selection['trees']:
        if (not isinstance(tree, dict) or set(tree) != {'path', 'exclude'} or
                not isinstance(tree['exclude'], list) or
                any(not isinstance(n, str) or not n or '/' in n for n in tree['exclude'])):
            raise ValueError('invalid selected bootstrap tree')
        walk(canonical_path(tree['path']), set(tree['exclude']))
    seen = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ValueError('bootstrap dependency time budget exceeded')
        result = subprocess.run([ldd, str(path)], env=ENVIRONMENT,
                                capture_output=True, text=True, timeout=min(10, remaining))
        if (result.returncode or len(result.stdout) + len(result.stderr) > 1024 * 1024 or
                'not found' in result.stdout):
            raise ValueError('bootstrap runtime dependency observation failed: ' + str(path))
        for line in result.stdout.splitlines():
            match = re.fullmatch(r'\s*\S+\s+=>\s+(/\S+)\s+\(0x[0-9a-fA-F]+\)\s*', line)
            if '=>' in line and not match:
                raise ValueError('unsupported bootstrap dependency observation row')
            if match:
                dependency = canonical_path(match.group(1))
                if not any(dependency.is_relative_to(root) for root in libraries):
                    raise ValueError('bootstrap runtime escaped selected library roots')
                add(dependency)
                edges.append({'consumer': str(path), 'dependency': str(dependency)})
        if stamp(path.lstat()) != stamps[str(path)]:
            raise ValueError('bootstrap executable changed during dependency observation')
    for name, before in stamps.items():
        if stamp(Path(name).lstat()) != before:
            raise ValueError('bootstrap input changed before inventory publication')
    data = {'schema': 'daemonarchy/bootstrap-toolchain-inventory@1',
            'scope': 'read-only-inventory', 'admitted': False,
            'source_build_provenance': 'unknown-host-bootstrap',
            'linker_scripts': scripts, 'entries': dict(sorted(entries.items()))}
    observation = {'schema': 'daemonarchy/bootstrap-dependency-observation@1',
                   'admitted': False, 'entries': len(entries), 'file_bytes': total,
                   'elf_objects': len(seen), 'runtime_edges': edges}
    return data, observation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--selection', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--observation', type=Path, required=True)
    args = parser.parse_args()
    with args.selection.open('rb') as stream:
        raw = stream.read(8 * 1024 * 1024 + 1)
    if len(raw) > 8 * 1024 * 1024:
        raise ValueError('bootstrap selection metadata budget exceeded')
    selected = json.loads(raw, object_pairs_hook=unique_object)
    data, observed = inventory(selected)
    observed['selection_sha256'] = hashlib.sha256(raw).hexdigest()
    content = (json.dumps(data, sort_keys=True, indent=2) + '\n').encode()
    observed['inventory_sha256'] = hashlib.sha256(content).hexdigest()
    with args.output.open('xb') as stream:
        stream.write(content)
    with args.observation.open('x') as stream:
        stream.write(json.dumps(observed, sort_keys=True, indent=2) + '\n')
    print(json.dumps({k: v for k, v in observed.items() if k != 'runtime_edges'}))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError, subprocess.SubprocessError) as error:
        print(json.dumps({'status': 'refused', 'admitted': False, 'error': str(error)}))
        raise SystemExit(1)
