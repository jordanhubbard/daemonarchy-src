#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Translate exact original Kyua v3 results; never authorize skips or acceptance."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import stat
import tempfile
import time

MAX_DATABASE = 64 * 1024 * 1024
MAX_INVENTORY = 8 * 1024 * 1024
MAX_CASES = 100000
TABLES = {'metadata', 'contexts', 'env_vars', 'metadatas', 'test_programs',
          'test_cases', 'test_results', 'test_case_files', 'files', 'sqlite_sequence'}


def stamp(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def read_regular(path, limit):
    path = Path(path).absolute()
    if path.resolve(strict=True) != path:
        raise ValueError('report input must not be redirected')
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= limit:
        raise ValueError('report input type or byte budget exceeded')
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        if stamp(os.fstat(stream.fileno())) != stamp(before):
            raise ValueError('report input replaced before read')
        content = stream.read(before.st_size + 1)
        if stamp(os.fstat(stream.fileno())) != stamp(before):
            raise ValueError('report input changed during read')
    if (len(content) != before.st_size or stamp(path.lstat()) != stamp(before) or
            path.resolve(strict=True) != path):
        raise ValueError('report input changed during read')
    return content


def unique_object(items):
    result = {}
    for key, value in items:
        if key in result:
            raise ValueError('duplicate inventory JSON key')
        result[key] = value
    return result


def case_name(value):
    if (not isinstance(value, str) or not 0 < len(value) <= 1024 or
            any(c.isspace() or ord(c) < 32 or ord(c) == 127 for c in value)):
        raise ValueError('invalid original case identifier')
    return value


def program_name(value):
    case_name(value)
    if value.startswith('/') or any(p in ('', '.', '..') for p in value.split('/')) or '\\' in value:
        raise ValueError('test program must be a canonical relative path')
    return value


def inventory_cases(content):
    data = json.loads(content, object_pairs_hook=unique_object)
    if (not isinstance(data, dict) or set(data) != {'schema', 'cases'} or
            data['schema'] != 'daemonarchy/kyua-case-inventory@1' or
            not isinstance(data['cases'], list) or not 0 < len(data['cases']) <= MAX_CASES):
        raise ValueError('invalid original case inventory')
    selected = {}
    ids = []
    for item in data['cases']:
        if not isinstance(item, dict) or set(item) != {'program', 'case', 'id'}:
            raise ValueError('invalid original case binding')
        key = (program_name(item['program']), case_name(item['case']))
        identity = case_name(item['id'])
        if key in selected:
            raise ValueError('duplicate original test case')
        selected[key] = identity
        ids.append(identity)
    if ids != sorted(set(ids)):
        raise ValueError('case identities must be sorted and unique')
    return selected


def parse_results(database_bytes, selected):
    if not database_bytes.startswith(b'SQLite format 3\0'):
        raise ValueError('original results are not a SQLite database')
    if database_bytes[18:20] != b'\x01\x01':
        raise ValueError('original results must be self-contained rollback-mode SQLite')
    with tempfile.TemporaryDirectory(prefix='daemonarchy-kyua-report-') as temporary:
        path = Path(temporary) / 'results.db'
        path.write_bytes(database_bytes)
        path.chmod(0o400)
        connection = sqlite3.connect(path.as_uri() + '?mode=ro&immutable=1', uri=True)
        deadline = time.monotonic() + 10
        steps = 0
        def progress():
            nonlocal steps
            steps += 1000
            return steps > 10000000 or time.monotonic() > deadline
        connection.set_progress_handler(progress, 1000)
        try:
            connection.execute('PRAGMA query_only = ON')
            connection.execute('PRAGMA trusted_schema = OFF')
            objects = connection.execute('SELECT type,name,sql FROM sqlite_master').fetchmany(1025)
            if len(objects) > 1024:
                raise ValueError('original results schema object budget exceeded')
            tables = set()
            for kind, name, sql in objects:
                if kind == 'table':
                    if name not in TABLES or not isinstance(sql, str) or 'VIRTUAL' in sql.upper():
                        raise ValueError('unsupported original results table')
                    tables.add(name)
                elif kind != 'index':
                    raise ValueError('original results views or triggers are unsupported')
            if tables != TABLES:
                raise ValueError('incomplete Kyua v3 schema')
            versions = connection.execute('SELECT schema_version FROM metadata ORDER BY schema_version LIMIT 17').fetchall()
            if not versions or len(versions) > 16 or versions[-1] != (3,) or any(
                    type(v) is not int or not 1 <= v <= 3 for (v,) in versions):
                raise ValueError('unsupported Kyua schema version')
            if connection.execute('PRAGMA quick_check').fetchmany(2) != [('ok',)]:
                raise ValueError('original results database integrity failed')
            if connection.execute('PRAGMA foreign_key_check').fetchone() is not None:
                raise ValueError('orphan original test result')
            count = connection.execute('SELECT count(*) FROM test_cases').fetchone()[0]
            if count != len(selected) or connection.execute('SELECT count(*) FROM test_results').fetchone()[0] != count:
                raise ValueError('original case/result inventory mismatch')
            programs = connection.execute('SELECT relative_path FROM test_programs').fetchmany(MAX_CASES + 1)
            expected_programs = {p for p, _ in selected}
            if len(programs) != len(expected_programs) or {p for (p,) in programs} != expected_programs:
                raise ValueError('original test program inventory mismatch')
            rows = connection.execute(
                'SELECT p.relative_path,c.name,r.result_type,r.result_reason,r.start_time,r.end_time '
                'FROM test_cases c LEFT JOIN test_programs p ON p.test_program_id=c.test_program_id '
                'LEFT JOIN test_results r ON r.test_case_id=c.test_case_id').fetchmany(MAX_CASES + 1)
            if len(rows) != count:
                raise ValueError('incomplete original result joins')
            outcomes = []
            raw = []
            seen = set()
            for program, name, kind, reason, start, end in rows:
                key = (program, name)
                if key not in selected or key in seen:
                    raise ValueError('duplicate or unselected original result')
                seen.add(key)
                if (type(start) is not int or type(end) is not int or not 0 <= start <= end or
                        kind not in {'passed', 'failed', 'skipped', 'broken', 'expected_failure'}):
                    raise ValueError('invalid original result type or timing')
                if kind == 'passed':
                    if reason is not None:
                        raise ValueError('passed original result has a reason')
                elif not isinstance(reason, str) or not reason.strip() or len(reason) > 4096:
                    raise ValueError('original non-pass result requires a bounded reason')
                if kind == 'skipped' and (len(reason) > 256 or any(ord(c) < 32 for c in reason)):
                    raise ValueError('skip reason cannot enter retained policy unchanged')
                outcome = 'passed' if kind == 'passed' else 'skipped' if kind == 'skipped' else 'failed'
                outcomes.append({'id': selected[key], 'outcome': outcome,
                                 'reason': reason if kind == 'skipped' else None})
                raw.append({'id': selected[key], 'original_outcome': kind, 'original_reason': reason})
            return sorted(outcomes, key=lambda v: v['id']), sorted(raw, key=lambda v: v['id'])
        finally:
            connection.close()


def convert(results, inventory, action):
    case_name(action)
    if len(action) > 256:
        raise ValueError('retained action identifier budget exceeded')
    for suffix in ('-wal', '-journal', '-shm'):
        companion = Path(str(results) + suffix)
        if companion.exists() or companion.is_symlink():
            raise ValueError('original results have an unsealed SQLite companion')
    database_bytes = read_regular(results, MAX_DATABASE)
    inventory_bytes = read_regular(inventory, MAX_INVENTORY)
    cases, originals = parse_results(database_bytes, inventory_cases(inventory_bytes))
    report = {'schema': 'literate-ai/retained-project-test-report@1', 'action': action, 'cases': cases}
    report_bytes = (json.dumps(report, sort_keys=True, separators=(',', ':')) + '\n').encode()
    if len(report_bytes) > 16 * 1024 * 1024:
        raise ValueError('retained report byte budget exceeded')
    provenance = {'schema': 'daemonarchy/kyua-report-provenance@1',
                  'database_sha256': hashlib.sha256(database_bytes).hexdigest(),
                  'inventory_sha256': hashlib.sha256(inventory_bytes).hexdigest(),
                  'report_sha256': hashlib.sha256(report_bytes).hexdigest(),
                  'cases': originals, 'acceptance_receipt': False}
    return report_bytes, (json.dumps(provenance, sort_keys=True, separators=(',', ':')) + '\n').encode()


def write_new(path, content):
    path = Path(path).absolute()
    if path.parent.resolve(strict=True) != path.parent:
        raise ValueError('report output parent is redirected')
    with path.open('xb') as stream:
        stream.write(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--action', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--provenance', type=Path, required=True)
    args = parser.parse_args()
    report, provenance = convert(args.results, args.inventory, args.action)
    write_new(args.output, report)
    write_new(args.provenance, provenance)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError, KeyError, sqlite3.Error) as error:
        print(json.dumps({'status': 'refused', 'acceptance_receipt': False, 'error': str(error)}))
        raise SystemExit(1)
