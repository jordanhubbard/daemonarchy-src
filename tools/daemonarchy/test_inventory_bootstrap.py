#!/usr/bin/env python3
"""Real filesystem inventory and existing capture boundary checks."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


producer = load('bootstrap_inventory', 'inventory-bootstrap.py')
capture = load('bootstrap_capture', 'capture-seed.py')


class BootstrapInventoryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.inputs = self.root / 'inputs'
        self.inputs.mkdir()
        (self.inputs / 'file').write_bytes(b'original bootstrap input')
        self.selection = {'schema': 'daemonarchy/bootstrap-toolchain-selection@1',
                          'files': [], 'trees': [{'path': str(self.inputs), 'exclude': []}],
                          'library_roots': []}

    def test_inventory_copies_through_existing_capture_without_admission(self):
        (self.inputs / 'alias').symlink_to('file')
        (self.inputs / 'file').chmod(0o755)
        data, observed = producer.inventory(self.selection)
        content = (json.dumps(data, sort_keys=True, indent=2) + '\n').encode()
        path = self.root / 'inventory.json'
        path.write_bytes(content)
        output = self.root / 'captured'
        result = capture.capture(path, hashlib.sha256(content).hexdigest(), Path('/'), output)
        copied = output / 'tree' / str(self.inputs).lstrip('/')
        self.assertEqual((copied / 'file').read_bytes(), b'original bootstrap input')
        self.assertEqual((copied / 'alias').readlink(), Path('file'))
        self.assertEqual((copied / 'file').stat().st_mode & 0o777, 0o755)
        self.assertFalse(data['admitted'])
        self.assertFalse(result['admitted'])
        self.assertEqual(observed['file_bytes'], len(b'original bootstrap input'))

    def test_changed_bytes_refuse_existing_capture_and_preserve_failure(self):
        data, _ = producer.inventory(self.selection)
        content = json.dumps(data).encode()
        path = self.root / 'inventory.json'
        path.write_bytes(content)
        (self.inputs / 'file').write_bytes(b'changed bootstrap input!')
        output = self.root / 'failed-capture'
        with self.assertRaises(ValueError):
            capture.capture(path, hashlib.sha256(content).hexdigest(), Path('/'), output)
        self.assertTrue(output.exists())

    def test_excluded_subtree_is_not_captured(self):
        private = self.inputs / 'site-packages'
        private.mkdir()
        (private / 'unselected').write_bytes(b'not a bootstrap tool')
        self.selection['trees'][0]['exclude'] = ['site-packages']
        data, _ = producer.inventory(self.selection)
        self.assertNotIn(str(private), data['entries'])
        self.assertNotIn(str(private / 'unselected'), data['entries'])

    def test_linker_script_references_are_complete_and_cycles_refuse(self):
        script = self.inputs / 'libc.so'
        script.write_text('GROUP ( ' + str(self.inputs / 'file') + ' )\n')
        self.selection['library_roots'] = [str(self.inputs)]
        self.selection['files'] = [str(script)]
        self.selection['trees'] = []
        data, _ = producer.inventory(self.selection)
        self.assertIn(str(self.inputs / 'file'), data['entries'])
        self.assertEqual(data['linker_scripts'][str(script)]['references'],
                         [str(self.inputs / 'file')])
        script.write_text('GROUP ( ' + str(script) + ' )\n')
        with self.assertRaisesRegex(ValueError, 'cycle'):
            producer.inventory(self.selection)

    def test_redirected_ancestors_absolute_links_and_unknown_fields_refuse(self):
        redirect = self.root / 'redirect'
        redirect.symlink_to(self.inputs, target_is_directory=True)
        self.selection['files'] = [str(redirect / 'file')]
        self.selection['trees'] = []
        with self.assertRaisesRegex(ValueError, 'redirected'):
            producer.inventory(self.selection)
        self.selection['files'] = []
        self.selection['trees'] = [{'path': str(self.inputs), 'exclude': []}]
        (self.inputs / 'absolute').symlink_to(self.inputs / 'file')
        with self.assertRaisesRegex(ValueError, 'absolute bootstrap symlink'):
            producer.inventory(self.selection)
        self.selection['unexpected'] = True
        with self.assertRaisesRegex(ValueError, 'invalid bootstrap selection'):
            producer.inventory(self.selection)


if __name__ == '__main__':
    unittest.main()
