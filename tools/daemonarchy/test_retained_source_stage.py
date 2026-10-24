"""Require exact original listings before executing any selected test suite."""
import importlib.util
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('retained_stage', Path(__file__).with_name('retained-source-stage.py'))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)
HEADER = 'Content-Type: application/X-atf-tp; version="1"\n\n'


class OriginalCaseListingTests(unittest.TestCase):
    def test_secondary_paths_require_complete_canonical_disjoint_arguments(self):
        empty = dict(runtime_view=None, product_view=None, fixture_view=None, secondary_user=None)
        self.assertIsNone(stage.secondary_paths(SimpleNamespace(**empty)))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            paths = tuple(root / name for name in ('runtime', 'products', 'work'))
            for path in paths:
                path.mkdir()
            arguments = dict(zip(('runtime_view', 'product_view', 'fixture_view'), paths))
            arguments['secondary_user'] = 'litaitest'
            self.assertEqual(stage.secondary_paths(SimpleNamespace(**arguments)), paths)
            for key in empty:
                with self.subTest(key=key), self.assertRaises(ValueError):
                    stage.secondary_paths(SimpleNamespace(**{**arguments, key: None}))
            for changes in ({'secondary_user': 'invalid name'}, {'product_view': paths[0]},
                            {'product_view': root}, {'fixture_view': Path('relative')}):
                with self.subTest(changes=changes), self.assertRaises(ValueError):
                    stage.secondary_paths(SimpleNamespace(**{**arguments, **changes}))

    def test_selected_launcher_uses_only_reviewed_views_for_tools_and_loader(self):
        seed, products = Path('/reviewed/runtime'), Path('/reviewed/products')
        variables = stage.launcher_variables(seed, products, scoped=True)
        self.assertEqual(variables['ATF_SHELL'], '/reviewed/runtime/bin/sh')
        self.assertEqual(variables['LD_LIBRARY_PATH'], '/reviewed/runtime/lib:/reviewed/runtime/usr/lib:/reviewed/runtime/usr/local/lib')
        self.assertTrue(variables['PATH'].startswith('/reviewed/products/bin:'))
        self.assertNotIn('LD_LIBRARY_PATH', stage.launcher_variables(seed, products))

    def test_new_export_directories_are_readable_under_private_umask(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            root.chmod(0o700)
            target = root / 'products/tests'
            previous = os.umask(0o077)
            try:
                stage.product_directory(target)
            finally:
                os.umask(previous)
            self.assertEqual(root.stat().st_mode & 0o777, 0o700)
            self.assertEqual(target.stat().st_mode & 0o777, 0o755)
            self.assertEqual(target.parent.stat().st_mode & 0o777, 0o755)

    def test_original_runner_receives_reviewed_account_views_and_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            inputs, objects, outputs, runtime, products, fixture = (
                root / name for name in ('inputs', 'objects', 'outputs', 'runtime', 'products', 'fixture'))
            for path in (inputs, objects, outputs, runtime, products, fixture):
                path.mkdir()
            inventory = inputs / 'tools/daemonarchy/retained/original-cases.json'
            inventory.parent.mkdir(parents=True)
            inventory.write_text(json.dumps({'cases':[{'program':'truncate_test', 'case':'sample', 'id':'sample'}]}))
            argv = ['retained-source-stage.py', 'tests', '--inputs', str(inputs),
                    '--objects', str(objects), '--outputs', str(outputs),
                    '--runtime-view', str(runtime), '--product-view', str(products),
                    '--fixture-view', str(fixture), '--secondary-user', 'litaitest']
            with patch.object(sys, 'argv', argv), \
                 patch.object(stage.subprocess, 'check_output', return_value=HEADER+'ident: sample\n') as listing, \
                 patch.object(stage, 'run') as run:
                stage.main()
            self.assertEqual(listing.call_args.args[0], [str(products / 'tests/truncate_test'), '-l'])
            self.assertEqual(len(run.call_args_list), 2)
            runner = run.call_args_list[0]
            self.assertEqual(runner.args[0][0], runtime / 'usr/bin/kyua')
            self.assertIn('--variable=unprivileged_user=litaitest', runner.args[0])
            self.assertIn('--build-root='+str(products / 'tests'), runner.args[0])
            self.assertEqual(runner.kwargs['env']['TMPDIR'], str(fixture))
            self.assertEqual(runner.kwargs['env']['ATF_SHELL'], str(runtime / 'bin/sh'))
            self.assertTrue(runner.kwargs['env']['LD_LIBRARY_PATH'].startswith(str(runtime / 'lib')))

    def test_compiler_link_plan_selects_the_exact_captured_linker(self):
        expected = Path('/private captured tools/ld.lld')
        listing = 'compiler version header\n "/captured/cc" "-cc1" "-x" "c"\n ' + \
                  '"/private captured tools/ld.lld" "--sysroot=/captured"\n'
        stage.require_linker_listing(listing, expected)

    def test_missing_foreign_ambiguous_or_malformed_link_jobs_refuse(self):
        for listing in ('compiler header only', '"/captured/cc" "-cc1"',
                        '"/usr/bin/ld" "-o" "program"',
                        '"/captured/ld.lld"\n"/captured/ld.lld"',
                        '"/captured/as"\n"/captured/ld.lld"', '"unterminated'):
            with self.subTest(listing=listing), self.assertRaises(ValueError):
                stage.require_linker_listing(listing, Path('/captured/ld.lld'))

    def test_metadata_is_preserved_and_the_exact_inventory_is_required(self):
        listing = HEADER + 'ident: first\ndescr: First original case\n\nident: second\nrequire.user: root\n'
        stage.require_case_listing(listing, {'first', 'second'})
        for selected in ({'first'}, {'first', 'second', 'third'}):
            with self.subTest(selected=selected), self.assertRaisesRegex(ValueError, 'differs'):
                stage.require_case_listing(listing, selected)

    def test_empty_duplicate_or_unsupported_listing_refuses(self):
        for listing in ('', HEADER, HEADER+'ident: first\nident: first\n',
                        HEADER.replace('version="1"','version="2"')+'ident: first\n'):
            with self.subTest(listing=listing), self.assertRaises(ValueError):
                stage.require_case_listing(listing, {'first'})


if __name__ == '__main__':
    unittest.main()
