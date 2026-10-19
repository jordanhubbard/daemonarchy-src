#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Copy an explicitly reviewed bootstrap inventory; never grant SDK admission."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import stat

MAX_METADATA = 8 * 1024 * 1024
MAX_ENTRIES = 10000
MAX_FILE = 128 * 1024 * 1024
MAX_TOTAL = 512 * 1024 * 1024


def unique_object(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError('duplicate inventory key')
        result[key] = value
    return result


def name_parts(name):
    if not isinstance(name, str) or not name.startswith('/') or name == '/':
        raise ValueError('inventory requires canonical absolute paths')
    parts = name[1:].split('/')
    if any(not part or part in ('.', '..') or '\\' in part or
           any(ord(c) < 32 or ord(c) == 127 for c in part) for part in parts):
        raise ValueError('inventory requires canonical absolute paths')
    return parts


def read_inventory(path, expected_sha256):
    if not re.fullmatch('[0-9a-f]{64}', expected_sha256):
        raise ValueError('reviewed inventory SHA-256 required')
    with Path(path).open('rb') as stream:
        content = stream.read(MAX_METADATA + 1)
    if len(content) > MAX_METADATA:
        raise ValueError('inventory metadata budget exceeded')
    if hashlib.sha256(content).hexdigest() != expected_sha256:
        raise ValueError('inventory digest differs from reviewed selection')
    data = json.loads(content, object_pairs_hook=unique_object)
    if not isinstance(data, dict) or set(data) != {
        'schema', 'scope', 'admitted', 'source_build_provenance',
        'linker_scripts', 'entries',
    }:
        raise ValueError('unexpected inventory fields')
    if (data['schema'] != 'daemonarchy/bootstrap-toolchain-inventory@1' or
            data['scope'] != 'read-only-inventory' or data['admitted'] is not False or
            data['source_build_provenance'] != 'unknown-host-bootstrap'):
        raise ValueError('inventory cannot grant provenance or admission')
    entries = data['entries']
    if not isinstance(entries, dict) or not 0 < len(entries) <= MAX_ENTRIES:
        raise ValueError('inventory entry budget exceeded')
    total = 0
    for name, item in entries.items():
        parts = name_parts(name)
        if not isinstance(item, dict):
            raise ValueError('invalid inventory entry')
        kind = item.get('kind')
        expected = {'kind', 'mode'} | {
            'file': {'size', 'sha256'}, 'directory': set(), 'symlink': {'target'},
        }.get(kind, {'unsupported'})
        if set(item) != expected or type(item['mode']) is not int or not 0 <= item['mode'] <= 0o777:
            raise ValueError('unsupported inventory entry or mode')
        for i in range(1, len(parts)):
            ancestor = entries.get('/' + '/'.join(parts[:i]))
            if ancestor is not None and ancestor['kind'] != 'directory':
                raise ValueError('inventory has non-directory ancestor')
        if kind == 'file':
            if (type(item['size']) is not int or not 0 <= item['size'] <= MAX_FILE or
                    not isinstance(item['sha256'], str) or
                    not re.fullmatch('[0-9a-f]{64}', item['sha256'])):
                raise ValueError('invalid file identity or file budget exceeded')
            total += item['size']
        if kind == 'symlink':
            target = item['target']
            if not isinstance(target, str) or not target or len(target.encode()) > 4096 or target.startswith('/'):
                raise ValueError('symlink requires bounded relative target')
            resolved = parts[:-1]
            for part in target.split('/'):
                if part == '..':
                    if not resolved:
                        raise ValueError('symlink escapes seed root')
                    resolved.pop()
                elif part not in ('', '.'):
                    name_parts('/' + part)
                    resolved.append(part)
            destination = '/' + '/'.join(resolved)
            # Resolve only directly captured files/directories. Chained links
            # need a separately reviewed extension, never ambient resolution.
            if destination not in entries or entries[destination]['kind'] not in ('file', 'directory'):
                raise ValueError('symlink target is not directly captured')
    if total > MAX_TOTAL:
        raise ValueError('aggregate seed byte budget exceeded')
    scripts = data['linker_scripts']
    if not isinstance(scripts, dict):
        raise ValueError('invalid linker script inventory')
    for name, script in scripts.items():
        if (name not in entries or entries[name]['kind'] != 'file' or
                not isinstance(script, dict) or set(script) != {'sha256', 'references'} or
                script['sha256'] != entries[name]['sha256'] or
                not isinstance(script['references'], list) or not script['references']):
            raise ValueError('invalid linker script identity')
        for reference in script['references']:
            name_parts(reference)
            if reference not in entries or entries[reference]['kind'] not in ('file', 'symlink'):
                raise ValueError('uncaptured linker script reference')
    return content, data


@contextmanager
def parent_fd(root_fd, parts):
    descriptor = os.dup(root_fd)
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                            dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        yield descriptor
    finally:
        os.close(descriptor)


def stamp(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def copy_entry(root_fd, name, item, tree):
    parts = name_parts(name)
    destination = tree.joinpath(*parts)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with parent_fd(root_fd, parts) as parent:
        before = os.stat(parts[-1], dir_fd=parent, follow_symlinks=False)
        if stat.S_IMODE(before.st_mode) != item['mode']:
            raise ValueError('source mode changed: ' + name)
        kind = item['kind']
        if kind == 'directory':
            if not stat.S_ISDIR(before.st_mode):
                raise ValueError('source directory changed: ' + name)
            destination.mkdir(exist_ok=True)
        elif kind == 'symlink':
            if not stat.S_ISLNK(before.st_mode) or os.readlink(parts[-1], dir_fd=parent) != item['target']:
                raise ValueError('source symlink changed: ' + name)
            destination.symlink_to(item['target'])
        else:
            if not stat.S_ISREG(before.st_mode) or before.st_size != item['size']:
                raise ValueError('source file type or size changed: ' + name)
            fd = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
            digest = hashlib.sha256()
            count = 0
            with os.fdopen(fd, 'rb') as source, destination.open('xb') as output:
                if stamp(os.fstat(source.fileno())) != stamp(before):
                    raise ValueError('source replaced before read: ' + name)
                while chunk := source.read(min(1024 * 1024, item['size'] - count + 1)):
                    count += len(chunk)
                    if count > item['size']:
                        raise ValueError('source grew during read: ' + name)
                    digest.update(chunk)
                    output.write(chunk)
                if stamp(os.fstat(source.fileno())) != stamp(before):
                    raise ValueError('source changed during read: ' + name)
            if count != item['size'] or digest.hexdigest() != item['sha256']:
                raise ValueError('source digest changed: ' + name)
            destination.chmod(0o755 if item['mode'] & 0o111 else 0o644)
        if stamp(os.stat(parts[-1], dir_fd=parent, follow_symlinks=False)) != stamp(before):
            raise ValueError('source replaced during capture: ' + name)


def capture(inventory, reviewed_sha256, source_root, output):
    content, data = read_inventory(inventory, reviewed_sha256)
    source_root = Path(source_root).absolute()
    output = Path(output).absolute()
    if source_root.resolve(strict=True) != source_root or output.parent.resolve(strict=True) != output.parent:
        raise ValueError('source root and output parent must not be redirected')
    # A fresh owned directory is also the failure boundary. Never remove an
    # incomplete capture or replace an existing output on a retry.
    output.mkdir(mode=0o700)
    tree = output / 'tree'
    tree.mkdir(mode=0o700)
    root_fd = os.open(source_root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for name, item in sorted(data['entries'].items()):
            copy_entry(root_fd, name, item, tree)
    finally:
        os.close(root_fd)
    for name, script in data['linker_scripts'].items():
        path = tree.joinpath(*name_parts(name))
        if path.stat().st_size > 4096:
            raise ValueError('linker script budget exceeded')
        match = re.fullmatch(r'\s*GROUP\s*\(\s*((?:/[A-Za-z0-9_./+-]+\s*)+)\)\s*',
                             path.read_text(encoding='ascii'))
        if match is None or match.group(1).split() != script['references']:
            raise ValueError('linker script references differ from inventory')
    (output / 'inventory.json').write_bytes(content)
    result = {
        'schema': 'daemonarchy/bootstrap-seed-capture@1',
        'status': 'copied-seed', 'admitted': False,
        'source_build_provenance': 'unknown-host-bootstrap',
        'inventory_sha256': reviewed_sha256,
        'entries': len(data['entries']),
        'file_bytes': sum(v['size'] for v in data['entries'].values() if v['kind'] == 'file'),
        'normalized_file_modes': '0644-or-0755',
        'remaining': ['framework-input-admission', 'sdk-admission',
                      'relocated-compiler-and-sysroot-qualification'],
    }
    (output / 'capture.json').write_text(json.dumps(result, sort_keys=True, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--inventory-sha256', required=True)
    parser.add_argument('--source-root', type=Path, default=Path('/'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(capture(args.inventory, args.inventory_sha256, args.source_root, args.output), sort_keys=True))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError, KeyError) as error:
        print(json.dumps({'status': 'refused', 'admitted': False, 'error': str(error)}))
        raise SystemExit(1)
