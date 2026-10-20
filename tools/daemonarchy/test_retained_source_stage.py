"""Require exact original listings before executing any selected test suite."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('retained_stage', Path(__file__).with_name('retained-source-stage.py'))
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)
HEADER = 'Content-Type: application/X-atf-tp; version="1"\n\n'


class OriginalCaseListingTests(unittest.TestCase):
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
