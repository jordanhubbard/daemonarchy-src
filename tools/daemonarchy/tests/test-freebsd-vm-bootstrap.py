#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Fault controls against the actual pinned action checkout and release asset.

No VM starts. The input checkout is never modified; each case uses a private
sparse clone. Obtain the asset with the exact API command in the native workflow.
"""
import argparse
from contextlib import contextmanager
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HELPER = Path(__file__).resolve().parents[1]/'prepare-freebsd-vm-action.py'
spec = importlib.util.spec_from_file_location('bootstrap', HELPER)
bootstrap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootstrap)


class PinnedActionControls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.action = self.root/'action'
        subprocess.run(['git', 'clone', '--quiet', '--shared', '--no-checkout',
                        str(ACTION), str(self.action)], check=True)
        self.git('sparse-checkout', 'set', '--no-cone', '/index.js', '/LICENSE',
                 '/action.yml', '/package.json', '/conf/15.0.conf')
        self.git('checkout', '--quiet', '--detach', bootstrap.ACTION_COMMIT)
        self.runtime = self.root/'download.py'
        self.runtime.write_bytes(RUNTIME.read_bytes())
        self.receipt = self.root/'receipt.json'
        self.original = (self.action/'index.js').read_bytes()

    def git(self, *args):
        return bootstrap.git(self.action, *args)

    def prepare(self):
        return bootstrap.prepare(self.action, self.runtime, self.receipt)

    def refuse(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.prepare()
        self.assertEqual((self.action/'index.js').read_bytes(), self.original)
        self.assertFalse((self.action/'anyvm.py').exists())
        self.assertFalse(self.receipt.exists())

    def test_actual_pinned_source_and_asset_prepare(self):
        result = self.prepare()
        self.assertEqual(result['runtime_sha256'], bootstrap.RUNTIME_SHA256)
        self.assertEqual(result['builder_version'], '2.2.8')
        self.assertEqual((self.action/'anyvm.py').read_bytes(), RUNTIME.read_bytes())
        self.assertEqual(json.loads(self.receipt.read_text()), result)
        patched = (self.action/'index.js').read_bytes()
        self.assertNotIn(bootstrap.DOWNLOAD, patched)
        replacement = bootstrap.replacement().replace(b'\n', b'\r\n')
        self.assertEqual(patched.replace(replacement, bootstrap.DOWNLOAD), self.original)
        self.assertEqual(self.git('diff', '--name-only'), 'index.js')
        self.assertEqual((self.action/'LICENSE').read_bytes(),
                         subprocess.check_output(['git', '-C', str(self.action),
                                                  'show', 'HEAD:LICENSE']))
        subprocess.run(['node', '--check', str(self.action/'index.js')], check=True)

    def test_wrong_commit(self):
        self.git('-c', 'user.name=Bootstrap control', '-c',
                 'user.email=bootstrap-control@example.invalid', '-c',
                 'commit.gpgsign=false', 'commit',
                 '--quiet', '--allow-empty', '-m', 'Private wrong-pin control')
        self.original = (self.action/'index.js').read_bytes()
        self.refuse('HEAD differs')

    def test_dirty_tracked_dependency(self):
        with (self.action/'action.yml').open('ab') as stream:
            stream.write(b'\n# drift\n')
        self.refuse('tracked source is dirty')

    def test_index_hash_even_when_git_hides_change(self):
        self.git('update-index', '--assume-unchanged', 'index.js')
        (self.action/'index.js').write_bytes(self.original+b'\n// drift\n')
        self.original = (self.action/'index.js').read_bytes()
        self.refuse('index hash')

    def test_wrong_vm_runtime_pair(self):
        self.git('update-index', '--assume-unchanged', 'conf/15.0.conf')
        p = self.action/'conf/15.0.conf'
        p.write_bytes(p.read_bytes().replace(b'ANYVM_VERSION=0.7.2', b'ANYVM_VERSION=0.7.3'))
        self.refuse('runtime/VM pair differs')

    def test_license_preservation(self):
        self.git('update-index', '--assume-unchanged', 'LICENSE')
        (self.action/'LICENSE').write_text('replacement license')
        self.refuse('MIT license differs')

    def test_truncated_release_asset(self):
        self.runtime.write_bytes(self.runtime.read_bytes()[:-1])
        self.refuse('size/SHA256 mismatch')

    def test_same_size_modified_release_asset(self):
        data = self.runtime.read_bytes()
        self.runtime.write_bytes(bytes([data[0]^1])+data[1:])
        self.refuse('size/SHA256 mismatch')

    def test_refuse_existing_runtime(self):
        p = self.action/'anyvm.py'
        p.write_bytes(b'occupied')
        with self.assertRaisesRegex(ValueError, 'occupied output'):
            self.prepare()
        self.assertEqual(p.read_bytes(), b'occupied')
        self.assertEqual((self.action/'index.js').read_bytes(), self.original)
        self.assertFalse(self.receipt.exists())

    def test_refuse_dangling_output_symlink(self):
        p = self.action/'anyvm.py'
        p.symlink_to(self.root/'absent')
        with self.assertRaisesRegex(ValueError, 'occupied output'):
            self.prepare()
        self.assertTrue(p.is_symlink())
        self.assertFalse((self.root/'absent').exists())
        self.assertEqual((self.action/'index.js').read_bytes(), self.original)

    def test_refuse_occupied_receipt(self):
        self.receipt.write_bytes(b'preserve')
        with self.assertRaisesRegex(ValueError, 'occupied output'):
            self.prepare()
        self.assertEqual(self.receipt.read_bytes(), b'preserve')
        self.assertFalse((self.action/'anyvm.py').exists())
        self.assertEqual((self.action/'index.js').read_bytes(), self.original)

    def test_refuse_repreparation(self):
        self.prepare()
        patched = (self.action/'index.js').read_bytes()
        receipt = self.receipt.read_bytes()
        with self.assertRaisesRegex(ValueError, 'tracked source is dirty'):
            self.prepare()
        self.assertEqual((self.action/'index.js').read_bytes(), patched)
        self.assertEqual(self.receipt.read_bytes(), receipt)

    def test_refuse_runtime_input_symlink(self):
        self.runtime.unlink()
        self.runtime.symlink_to(RUNTIME)
        self.refuse('regular file')

    def test_failed_index_publication_rolls_back_owned_outputs(self):
        with patch.object(bootstrap.os, 'replace', side_effect=OSError('publication failed')):
            with self.assertRaisesRegex(OSError, 'publication failed'):
                self.prepare()
        self.assertEqual((self.action/'index.js').read_bytes(), self.original)
        self.assertFalse((self.action/'anyvm.py').exists())
        self.assertFalse(self.receipt.exists())
        self.assertEqual(list(self.action.glob('.prepared-index-*')), [])

    def test_partial_temporary_write_failure_removes_owned_file(self):
        original_create = bootstrap.tempfile.NamedTemporaryFile
        observed = []

        @contextmanager
        def failing_temporary(*args, **kwargs):
            with original_create(*args, **kwargs) as stream:
                observed.append(Path(stream.name))

                def partial_write(data):
                    stream.file.write(data[:32])
                    stream.file.flush()
                    self.assertEqual(observed[-1].stat().st_size, 32)
                    raise OSError('temporary write failed')

                with patch.object(stream, 'write', side_effect=partial_write):
                    yield stream

        with patch.object(bootstrap.tempfile, 'NamedTemporaryFile', failing_temporary):
            with self.assertRaisesRegex(OSError, 'temporary write failed'):
                self.prepare()
        self.assertEqual(len(observed), 1)
        self.assertFalse(observed[0].exists())
        self.assertEqual((self.action/'index.js').read_bytes(), self.original)
        self.assertFalse((self.action/'anyvm.py').exists())
        self.assertFalse(self.receipt.exists())
        self.assertEqual(list(self.action.glob('.prepared-index-*')), [])

    def test_execution_time_version_and_readback_guards(self):
        # Execute the exact replacement that is installed into upstream JS.
        source = ('import * as fs from "fs"; import * as crypto from "crypto";\n'
                  'const anyvmPath=process.argv[1]; const anyvmVersion=process.argv[2];\n'+
                  bootstrap.replacement().decode())
        def run(version):
            return subprocess.run(['node', '--input-type=module', '-e', source,
                                   str(self.runtime), version], capture_output=True, text=True)
        self.assertEqual(run('0.7.2').returncode, 0)
        wrong = run('0.7.3')
        self.assertNotEqual(wrong.returncode, 0)
        self.assertIn('version mismatch', wrong.stderr)
        data = self.runtime.read_bytes()
        for corrupt in (data[:-1], bytes([data[0]^1])+data[1:]):
            self.runtime.write_bytes(corrupt)
            failure = run('0.7.2')
            self.assertNotEqual(failure.returncode, 0)
            self.assertIn('size/SHA256 mismatch', failure.stderr)
        self.runtime.unlink()
        self.assertNotEqual(run('0.7.2').returncode, 0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--action-checkout', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    args = parser.parse_args()
    ACTION, RUNTIME = args.action_checkout.resolve(), args.runtime.resolve()
    unittest.main(argv=[__file__, '-v'])
