# SPDX-License-Identifier: BSD-2-Clause
"""Bootstrap byte custody and refusal tests; not native SDK qualification."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('capture_seed', Path(__file__).with_name('capture-seed.py'))
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


class SeedTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.host = self.root / 'host'
        self.host.mkdir()
        (self.host / 'lib').mkdir()
        (self.host / 'lib/runtime').write_bytes(b'actual runtime bytes')
        (self.host / 'lib/runtime').chmod(0o444)
        (self.host / 'lib/alias').symlink_to('runtime')
        (self.host / 'cc').write_bytes(b'compiler bytes')
        (self.host / 'cc').chmod(0o555)
        (self.host / 'script').write_text('GROUP ( /lib/alias )\n')
        entries = {}
        for name in ('cc', 'lib', 'lib/runtime', 'lib/alias', 'script'):
            path = self.host / name
            mode = stat.S_IMODE(path.lstat().st_mode)
            if path.is_symlink():
                item = {'kind': 'symlink', 'mode': mode, 'target': os.readlink(path)}
            elif path.is_dir():
                item = {'kind': 'directory', 'mode': mode}
            else:
                content = path.read_bytes()
                item = {'kind': 'file', 'mode': mode, 'size': len(content),
                        'sha256': hashlib.sha256(content).hexdigest()}
            entries['/' + name] = item
        self.data = {'schema': 'daemonarchy/bootstrap-toolchain-inventory@1',
                     'scope': 'read-only-inventory', 'admitted': False,
                     'source_build_provenance': 'unknown-host-bootstrap',
                     'entries': entries,
                     'linker_scripts': {'/script': {'sha256': entries['/script']['sha256'],
                                                    'references': ['/lib/alias']}}}
        self.inventory = self.root / 'inventory.json'
        self.output = self.root / 'capture'

    def write(self):
        content = json.dumps(self.data).encode()
        self.inventory.write_bytes(content)
        return hashlib.sha256(content).hexdigest()

    def capture(self):
        return seed.capture(self.inventory, self.write(), self.host, self.output)

    def test_exact_bytes_modes_links_and_no_admission(self):
        result = self.capture()
        self.assertFalse(result['admitted'])
        self.assertEqual(result['source_build_provenance'], 'unknown-host-bootstrap')
        self.assertEqual((self.output / 'tree/cc').read_bytes(), b'compiler bytes')
        self.assertEqual(stat.S_IMODE((self.output / 'tree/cc').stat().st_mode), 0o755)
        self.assertEqual(stat.S_IMODE((self.output / 'tree/lib/runtime').stat().st_mode), 0o644)
        self.assertEqual(os.readlink(self.output / 'tree/lib/alias'), 'runtime')
        self.assertEqual((self.output / 'tree/lib/alias').read_bytes(), b'actual runtime bytes')
        self.assertEqual((self.output / 'inventory.json').read_bytes(), self.inventory.read_bytes())
        self.assertEqual(stat.S_IMODE((self.host / 'cc').stat().st_mode), 0o555)

    def test_reviewed_digest_and_duplicate_keys_refuse_before_writes(self):
        self.write()
        with self.assertRaisesRegex(ValueError, 'digest'):
            seed.capture(self.inventory, '0' * 64, self.host, self.output)
        content = self.inventory.read_bytes().replace(b'"admitted": false', b'"admitted": false, "admitted": false')
        self.inventory.write_bytes(content)
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            seed.capture(self.inventory, hashlib.sha256(content).hexdigest(), self.host, self.output)
        self.assertFalse(self.output.exists())

    def test_growth_and_aggregate_budgets_refuse_before_output(self):
        with patch.object(seed, 'MAX_FILE', 1):
            with self.assertRaisesRegex(ValueError, 'file budget'):
                self.capture()
        with patch.object(seed, 'MAX_TOTAL', 1):
            with self.assertRaisesRegex(ValueError, 'aggregate'):
                self.capture()
        self.assertFalse(self.output.exists())

    def test_source_digest_drift_preserves_incomplete_capture(self):
        digest = self.write()
        (self.host / 'cc').chmod(0o755)
        (self.host / 'cc').write_bytes(b'wrong compiler')
        (self.host / 'cc').chmod(0o555)
        with self.assertRaisesRegex(ValueError, 'digest'):
            seed.capture(self.inventory, digest, self.host, self.output)
        self.assertTrue(self.output.is_dir())
        self.assertFalse((self.output / 'capture.json').exists())
        with self.assertRaises(FileExistsError):
            seed.capture(self.inventory, digest, self.host, self.output)

    def test_redirected_source_ancestor_refuses(self):
        digest = self.write()
        (self.host / 'lib').rename(self.host / 'other')
        (self.host / 'lib').symlink_to('other', target_is_directory=True)
        with self.assertRaises((OSError, ValueError)):
            seed.capture(self.inventory, digest, self.host, self.output)
        self.assertFalse((self.output / 'capture.json').exists())

    def test_escaping_or_missing_link_target_refuses_before_output(self):
        for target in ('../../escape', '/lib/runtime', 'missing'):
            with self.subTest(target=target):
                self.data['entries']['/lib/alias']['target'] = target
                with self.assertRaises(ValueError):
                    self.capture()
                self.assertFalse(self.output.exists())

    def test_unlisted_linker_reference_or_false_reference_list_refuses(self):
        self.data['linker_scripts']['/script']['references'] = ['/uncaptured']
        with self.assertRaisesRegex(ValueError, 'uncaptured'):
            self.capture()
        self.assertFalse(self.output.exists())
        self.data['linker_scripts']['/script']['references'] = ['/cc']
        with self.assertRaisesRegex(ValueError, 'references differ'):
            self.capture()
        self.assertFalse((self.output / 'capture.json').exists())

    def test_existing_output_and_redirected_parent_are_preserved(self):
        self.output.mkdir()
        marker = self.output / 'keep'
        marker.write_bytes(b'preserve')
        with self.assertRaises(FileExistsError):
            self.capture()
        self.assertEqual(marker.read_bytes(), b'preserve')
        (self.root / 'alias').symlink_to(self.root, target_is_directory=True)
        self.output = self.root / 'alias/new'
        with self.assertRaisesRegex(ValueError, 'redirected'):
            self.capture()
        self.assertFalse((self.root / 'new').exists())

    def test_special_source_file_and_privileged_mode_refuse(self):
        digest = self.write()
        (self.host / 'cc').unlink()
        os.mkfifo(self.host / 'cc', 0o555)
        with self.assertRaisesRegex(ValueError, 'type or size'):
            seed.capture(self.inventory, digest, self.host, self.output)
        self.data['entries']['/cc']['mode'] = 0o4555
        with self.assertRaisesRegex(ValueError, 'mode'):
            seed.read_inventory(self.inventory, self.write())

    def test_changed_open_file_refuses_and_retains_no_success_record(self):
        real = seed.os.fstat
        def changed(fd):
            result = real(fd)
            if stat.S_ISREG(result.st_mode):
                values = list(result)
                values[6] += 1
                return os.stat_result(values)
            return result
        with patch.object(seed.os, 'fstat', side_effect=changed):
            with self.assertRaisesRegex(ValueError, 'replaced before'):
                self.capture()
        self.assertFalse((self.output / 'capture.json').exists())


if __name__ == '__main__':
    unittest.main()
