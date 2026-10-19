# SPDX-License-Identifier: BSD-2-Clause
"""Original result-state custody; native Kyua execution is a separate check."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('report_kyua', Path(__file__).with_name('report-kyua.py'))
reporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reporter)
SCHEMA = Path(__file__).resolve().parents[2] / 'contrib/kyua/store/schema_v3.sql'


class KyuaReportTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.database = self.root / 'results.db'
        self.inventory = self.root / 'inventory.json'
        with sqlite3.connect(self.database) as db:
            db.executescript(SCHEMA.read_text())
            db.execute('INSERT INTO test_programs VALUES (1,?,?,?,?,?,?)',
                       ('/suite/tool', '/suite', 'tool', 'fixture', None, 'atf'))
            db.execute('INSERT INTO test_cases VALUES (1,1,?,NULL)', ('original_case',))
            db.execute('INSERT INTO test_results VALUES (1,?,NULL,10,20)', ('passed',))
        self.cases = [{'program': 'tool', 'case': 'original_case', 'id': 'unit.original_case'}]
        self.write_inventory()

    def write_inventory(self):
        self.inventory.write_text(json.dumps({'schema': 'daemonarchy/kyua-case-inventory@1',
                                              'cases': self.cases}))

    def change(self, sql, parameters=()):
        with sqlite3.connect(self.database) as db:
            db.execute(sql, parameters)

    def convert(self):
        result, origin = reporter.convert(self.database, self.inventory, 'original-tests')
        return json.loads(result), json.loads(origin)

    def test_exact_native_schema_preserves_pass_and_binds_original_bytes(self):
        before = self.database.read_bytes()
        raw, origin = reporter.convert(self.database, self.inventory, 'original-tests')
        report = json.loads(raw)
        self.assertEqual(set(report), {'schema', 'action', 'cases'})
        self.assertEqual(report['cases'], [{'id': 'unit.original_case', 'outcome': 'passed', 'reason': None}])
        provenance = json.loads(origin)
        self.assertEqual(provenance['database_sha256'], hashlib.sha256(before).hexdigest())
        self.assertEqual(provenance['report_sha256'], hashlib.sha256(raw).hexdigest())
        self.assertFalse(provenance['acceptance_receipt'])
        self.assertEqual(self.database.read_bytes(), before)

    def test_failed_broken_and_expected_failure_never_become_passes(self):
        for kind in ('failed', 'broken', 'expected_failure'):
            with self.subTest(kind=kind):
                self.change('UPDATE test_results SET result_type=?,result_reason=?', (kind, 'original reason'))
                report, provenance = self.convert()
                self.assertEqual(report['cases'][0]['outcome'], 'failed')
                self.assertIsNone(report['cases'][0]['reason'])
                self.assertEqual(provenance['cases'][0]['original_outcome'], kind)
                self.assertEqual(provenance['cases'][0]['original_reason'], 'original reason')

    def test_root_requirement_skip_is_exact_and_not_authorized_or_promoted(self):
        self.change('UPDATE test_results SET result_type=?,result_reason=?', ('skipped', 'Requires root privileges'))
        report, provenance = self.convert()
        self.assertEqual(report['cases'][0]['outcome'], 'skipped')
        self.assertEqual(report['cases'][0]['reason'], 'Requires root privileges')
        self.assertNotIn('skip_authorized', provenance)
        self.assertFalse(provenance['acceptance_receipt'])

    def test_missing_result_and_unselected_original_case_refuse(self):
        self.change('DELETE FROM test_results')
        with self.assertRaisesRegex(ValueError, 'inventory mismatch'):
            self.convert()
        self.change('INSERT INTO test_results VALUES (1,?,NULL,10,20)', ('passed',))
        self.change('UPDATE test_cases SET name=?', ('different_case',))
        with self.assertRaisesRegex(ValueError, 'unselected'):
            self.convert()

    def test_orphan_result_refuses(self):
        self.change('INSERT INTO test_results VALUES (99,?,NULL,10,20)', ('passed',))
        with self.assertRaisesRegex(ValueError, 'orphan'):
            self.convert()

    def test_duplicate_original_result_pair_cannot_fill_missing_case(self):
        self.cases.append({'program': 'tool', 'case': 'other_case', 'id': 'unit.other_case'})
        self.write_inventory()
        self.change('INSERT INTO test_cases VALUES (2,1,?,NULL)', ('original_case',))
        self.change('INSERT INTO test_results VALUES (2,?,NULL,10,20)', ('passed',))
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            self.convert()

    def test_schema_version_extra_objects_and_views_refuse(self):
        self.change('INSERT INTO metadata VALUES (4,1)')
        with self.assertRaisesRegex(ValueError, 'version'):
            self.convert()
        self.change('DELETE FROM metadata WHERE schema_version=4')
        self.change('CREATE VIEW substituted AS SELECT * FROM test_results')
        with self.assertRaisesRegex(ValueError, 'views'):
            self.convert()
        self.change('DROP VIEW substituted')
        self.change('CREATE TABLE unexpected(value TEXT)')
        with self.assertRaisesRegex(ValueError, 'unsupported'):
            self.convert()

    def test_unknown_state_reason_and_timing_refuse(self):
        for state, reason, start, end in (('unexpected', None, 10, 20), ('passed', 'reason', 10, 20),
                                         ('failed', None, 10, 20), ('passed', None, 20, 10),
                                         ('skipped', 'line\nbreak', 10, 20)):
            self.change('UPDATE test_results SET result_type=?,result_reason=?,start_time=?,end_time=?',
                        (state, reason, start, end))
            with self.subTest(state=state, reason=reason), self.assertRaises(ValueError):
                self.convert()

    def test_ambiguous_case_inventory_refuses(self):
        for key, value in (('id', 'has whitespace'), ('program', '../escape'), ('case', '')):
            original = self.cases[0][key]
            self.cases[0][key] = value
            self.write_inventory()
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.convert()
            self.cases[0][key] = original
        self.write_inventory()
        content = self.inventory.read_text().replace('"cases":', '"cases": [], "cases":')
        self.inventory.write_text(content)
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            self.convert()

    def test_redirected_input_byte_budget_and_sqlite_companions_refuse(self):
        alias = self.root / 'alias.db'
        alias.symlink_to(self.database.name)
        with self.assertRaisesRegex(ValueError, 'redirected'):
            reporter.convert(alias, self.inventory, 'original-tests')
        with patch.object(reporter, 'MAX_DATABASE', 1):
            with self.assertRaisesRegex(ValueError, 'budget'):
                self.convert()
        (self.root / 'results.db-wal').write_bytes(b'retain companion')
        with self.assertRaisesRegex(ValueError, 'companion'):
            self.convert()

    def test_existing_output_is_preserved(self):
        output = self.root / 'keep.json'
        output.write_bytes(b'keep original evidence')
        with self.assertRaises(FileExistsError):
            reporter.write_new(output, b'changed')
        self.assertEqual(output.read_bytes(), b'keep original evidence')

    @unittest.skipUnless(shutil.which('kyua'), 'requires actual native Kyua')
    def test_real_kyua_database_reports_actual_plain_test_outcome(self):
        program = self.root / 'pass.sh'
        program.write_text('#!/bin/sh\nexit 0\n')
        program.chmod(0o755)
        kyuafile = self.root / 'Kyuafile'
        kyuafile.write_text('syntax(2)\ntest_suite("report-fixture")\nplain_test_program{name="pass.sh"}\n')
        database = self.root / 'native.db'
        result = subprocess.run([shutil.which('kyua'), '-c', 'none', 'test',
                                 '--kyuafile=' + str(kyuafile), '--results-file=' + str(database)],
                                cwd=self.root, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.cases = [{'program': 'pass.sh', 'case': 'main', 'id': 'unit.plain'}]
        self.write_inventory()
        converted, origin = reporter.convert(database, self.inventory, 'original-tests')
        self.assertEqual(json.loads(converted)['cases'][0]['outcome'], 'passed')
        self.assertEqual(json.loads(origin)['cases'][0]['original_outcome'], 'passed')

    @unittest.skipUnless(sys.platform.startswith('freebsd') and shutil.which('kyua') and
                         Path('/usr/libexec/atf-sh').is_file() and os.geteuid() != 0,
                         'requires unprivileged native FreeBSD Kyua and ATF')
    def test_real_atf_expected_failure_and_root_skip_are_preserved(self):
        program = self.root / 'atf_fixture'
        program.write_text('''#!/usr/libexec/atf-sh
atf_test_case ok
ok_body() { :; }
atf_test_case expected
expected_body() { atf_expect_fail "Original expected failure"; atf_fail "observed failure"; }
atf_test_case privileged
privileged_head() { atf_set "require.user" "root"; }
privileged_body() { atf_fail "root-only body must not run unprivileged"; }
atf_init_test_cases() {
    atf_add_test_case ok
    atf_add_test_case expected
    atf_add_test_case privileged
}
''')
        program.chmod(0o755)
        kyuafile = self.root / 'Kyuafile'
        kyuafile.write_text('syntax(2)\ntest_suite("report-fixture")\natf_test_program{name="atf_fixture"}\n')
        database = self.root / 'native-atf.db'
        result = subprocess.run([shutil.which('kyua'), '-c', 'none', 'test',
                                 '--kyuafile=' + str(kyuafile), '--results-file=' + str(database)],
                                cwd=self.root, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.cases = [{'program': 'atf_fixture', 'case': name, 'id': 'unit.' + name}
                      for name in ('expected', 'ok', 'privileged')]
        self.write_inventory()
        raw, original = reporter.convert(database, self.inventory, 'original-tests')
        values = {case['id']: case for case in json.loads(raw)['cases']}
        self.assertEqual(values['unit.expected']['outcome'], 'failed')
        self.assertEqual(values['unit.ok']['outcome'], 'passed')
        self.assertEqual(values['unit.privileged']['outcome'], 'skipped')
        self.assertEqual(values['unit.privileged']['reason'], 'Requires root privileges')
        states = {case['id']: case['original_outcome'] for case in json.loads(original)['cases']}
        self.assertEqual(states['unit.expected'], 'expected_failure')


if __name__ == '__main__':
    unittest.main()
