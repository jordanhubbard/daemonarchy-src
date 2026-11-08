#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Controller refusal/data-oracle controls; never native DMA acceptance."""
import argparse
import ast
import hashlib
import secrets
import shlex
import subprocess
import struct
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('bcm2711-dma-native-control.py')
spec = importlib.util.spec_from_file_location('native_control', SCRIPT)
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


def zone(limit=0x3c000fff, alignment=4096, pages=1024):
    return {'lowaddr': limit, 'alignment': alignment, 'total_bpages': pages,
            'free_bpages': pages, 'reserved_bpages': 0, 'active_bpages': 0,
            'total_deferred': 0}


class ControllerControls(unittest.TestCase):
    def test_actual_sysctl_format(self):
        source = '\n'.join(f'hw.busdma.zone0.{k}: {hex(v) if k == "lowaddr" else v}'
                           for k, v in zone().items())
        self.assertEqual(control.zones(source+'\nhw.busdma.zone0.total_deferred_time: 0.000001'), {'zone0': zone()})

    def test_missing_observations_refuse(self):
        for source in ('', 'hw.busdma.total_bpages: 0', 'hw.busdma.zone0.lowaddr: 0x3fffffff'):
            with self.assertRaises(ValueError): control.zones(source)

    def test_observed_private_zone(self):
        before = {'zone0': zone()}
        after = dict(before, zone1=zone(control.PRIVATE_LIMIT, pages=4360))
        self.assertEqual(control.isolated_zone(before, after, 4096), 'zone1')

    def test_stricter_existing_zone_refuses(self):
        before = {'zone0': zone(0x0fffffff)}
        after = dict(before, zone1=zone(control.PRIVATE_LIMIT))
        with self.assertRaisesRegex(ValueError, 'shared'):
            control.isolated_zone(before, after, 4096)

    def test_missing_extra_wrong_or_oversized_zone_refuses(self):
        before = {'zone0': zone()}
        for after in (before, dict(before, zone1=zone(control.PRIVATE_LIMIT), zone2=zone()),
                      dict(before, zone1=zone(0x2fffffff)),
                      dict(before, zone1=zone(control.PRIVATE_LIMIT, pages=8193))):
            with self.assertRaises(ValueError): control.isolated_zone(before, after, 4096)

    def test_reserved_active_or_missing_pages_refuse(self):
        for changes in ({'reserved_bpages': 1}, {'active_bpages': 1}, {'free_bpages': 7}):
            value = dict(zone(), **changes)
            with self.assertRaises(ValueError): control.check_free(value)

    def test_usb_backend_is_bound_to_actual_controller_branch(self):
        qtree = ('  bus: bcm-xhci.0\n    dev: usb-storage, id "stick"\n'
                 '      drive = "dmadisk"\n  bus: other\n')
        self.assertTrue(control.usb_scratch_bound(qtree))
        self.assertFalse(control.usb_scratch_bound(qtree.replace('dmadisk', 'wrong')))
        self.assertFalse(control.usb_scratch_bound('  bus: bcm-xhci.0\n'
            '  bus: other\n    dev: usb-storage, id "stick"\n      drive = "dmadisk"\n'))
        self.assertFalse(control.usb_scratch_bound('  bus: bcm-xhci.0\n'
            '    dev: usb-storage, id "one"\n      drive = "wrong"\n'
            '    dev: virtio-blk, id "other"\n      drive = "dmadisk"\n'))

    def test_sd_geom_chain_uses_disk_ancestry(self):
        xml = ('<mesh><class><name>LABEL</name><geom><consumer><provider ref="p"/></consumer>'
               '<provider id="l"><name>gpt/HWTROOT</name></provider></geom></class>'
               '<class><name>PART</name><geom><consumer><provider ref="d"/></consumer>'
               '<provider id="p"><name>mmcsd0p2</name></provider></geom></class>'
               '<class><name>DISK</name><geom><provider id="d"><name>mmcsd0</name>'
               '</provider></geom></class></mesh>')
        self.assertEqual(control.root_disk(xml+'\0', '/dev/gpt/HWTROOT'), {'mmcsd0'})
        self.assertEqual(control.root_disk(xml.replace('mmcsd0</name>', 'da0</name>'),
                                           '/dev/gpt/HWTROOT'), {'da0'})
        self.assertEqual(control.root_disk(xml.replace('ref="d"', 'ref="l"'),
                                           '/dev/gpt/HWTROOT'), set())
        self.assertEqual(control.root_disk(xml.replace('ref="d"', 'ref="missing"'),
                                           '/dev/gpt/HWTROOT'), set())
        self.assertEqual(control.root_disk(xml.replace('id="d"', 'id="p"'),
                                           '/dev/gpt/HWTROOT'), set())

    def test_actual_sd_overlay_backing_and_only_usb_scratch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = {'scratch': str(root/'scratch.raw'), 'source_image': str(root/'source.img')}
            sd = root/'disk.qcow2'
            blocks = [{'device': 'dmadisk', 'inserted': {'file': inputs['scratch'],
                'image': {'virtual-size': control.CAPACITY, 'format': 'raw'}}},
                {'device': 'sd0', 'inserted': {'file': str(sd), 'image': {
                    'format': 'qcow2', 'full-backing-filename': inputs['source_image']}}}]
            control.validate_blocks(blocks, inputs, sd)
            import copy
            for kind in ('extra', 'overlay', 'backing', 'scratch', 'size'):
                changed = copy.deepcopy(blocks)
                if kind == 'extra': changed.append(copy.deepcopy(changed[0]))
                elif kind == 'overlay': changed[1]['inserted']['file'] = str(root/'wrong.qcow2')
                elif kind == 'backing': changed[1]['inserted']['image']['full-backing-filename'] = str(root/'wrong.img')
                elif kind == 'scratch': changed[0]['inserted']['file'] = str(root/'wrong.raw')
                else: changed[0]['inserted']['image']['virtual-size'] -= 512
                with self.assertRaises(ValueError): control.validate_blocks(changed, inputs, sd)

    def test_actual_generated_remote_shell_framing(self):
        tree = ast.parse(SCRIPT.read_text())
        node = next(node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef) and node.name == 'remote')
        namespace = {'ssh': [], 'shlex': shlex, 'secrets': secrets,
                     'record': lambda argv, timeout: subprocess.check_output(['sh', '-c', argv[-1]], text=True).strip()}
        exec(compile(ast.Module(body=[node], type_ignores=[]), str(SCRIPT), 'exec'), namespace)
        self.assertEqual(namespace['remote']('printf', '%s', 'actual shell output'), 'actual shell output')
        namespace['record'] = lambda argv, timeout: 'unframed output'
        with self.assertRaises(ValueError): namespace['remote']('true')

    def interrupt_blobs(self, count=7):
        names = (bytes(39)+b'gic0,s143: xhci0'.ljust(38, b' ')+b'\0'+
                 b'stray gic0,s143: xhci0'.ljust(38, b' ')+b'\0'+bytes(39))
        return names, struct.pack('<QQQQ', 0, count, 2, 0)

    def test_native_interrupt_slots_padding_and_u_long64(self):
        names, counts = self.interrupt_blobs((1 << 40)+7)
        rows = control.interrupts(names, counts)
        self.assertEqual(rows[0]['name'], '')
        self.assertEqual(rows[1], {'index': 1, 'name': 'gic0,s143: xhci0', 'count': (1 << 40)+7})
        before = control.interrupts(*self.interrupt_blobs(7))
        after = control.interrupts(*self.interrupt_blobs(29))
        result = control.xhci_interrupt_progress(before, after)
        self.assertEqual(result['delta'], 22)
        self.assertEqual(result['stray_before'], 2)
        self.assertEqual(result['stray_delta'], 0)

    def test_native_interrupt_vector_or_padding_refuses(self):
        names, counts = self.interrupt_blobs()
        for bad_names, bad_counts in ((names, b''), (names, counts[:-1]),
                                     (names[:-1], counts), (bytes(120), counts),
                                     (names[:39]+b'A\0'+names[41:], counts),
                                     (names[:39]+b'\xff'+names[40:], counts),
                                     (names[:-1]+b'X', counts)):
            with self.assertRaises(ValueError): control.interrupts(bad_names, bad_counts)

    def test_native_interrupt_missing_duplicate_changed_or_no_progress_refuses(self):
        before = control.interrupts(*self.interrupt_blobs(7))
        for after in (control.interrupts(*self.interrupt_blobs(7)),
                      control.interrupts(*self.interrupt_blobs(3)),
                      [dict(row, name='other') for row in before],
                      [dict(row, index=9) if row['name'] else row for row in before],
                      before+[dict(before[1])]):
            with self.assertRaises(ValueError): control.xhci_interrupt_progress(before, after)

    def test_measured_native_normal_and_bare_stray_pair(self):
        path = SCRIPT.parent/'fixtures/bcm2711-native-irq.json'
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),
                         '9c81676b958f2717e1105d2f33226d78a28f63a461712a1fd8116cae320d7515')
        fixture = json.loads(path.read_text())
        before = control.interrupts(bytes.fromhex(fixture['intrnames-before_hex']),
                                    bytes.fromhex(fixture['intrcnt-before_hex']))
        after = control.interrupts(bytes.fromhex(fixture['intrnames-after_hex']),
                                   bytes.fromhex(fixture['intrcnt-after_hex']))
        self.assertEqual(before[0]['count'], 232)
        self.assertEqual(after[0]['count'], 660)
        self.assertEqual(before[1]['name'], 'stray gic0,s143:')
        result = control.xhci_interrupt_progress(before, after)
        self.assertEqual(result['delta'], 428)
        self.assertEqual(result['stray_delta'], 0)
        wrong = [dict(row) for row in after]
        wrong[1]['name'] = 'stray gic0,s144:'
        with self.assertRaises(ValueError): control.xhci_interrupt_progress(before, wrong)

    def test_loaded_and_unloaded_linker_filenames_match(self):
        paths = {}
        for node in ast.walk(ast.parse(SCRIPT.read_text())):
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and
                    node.func.id == 'remote' and len(node.args) >= 3 and
                    isinstance(node.args[1], ast.Constant) and
                    node.args[1].value in ('kldload', 'kldunload')):
                self.assertNotIn(node.args[1].value, paths)
                paths[node.args[1].value] = ast.dump(node.args[2])
        self.assertEqual(set(paths), {'kldload', 'kldunload'})
        self.assertEqual(paths['kldload'], paths['kldunload'])
        self.assertEqual(paths['kldload'], ast.dump(ast.Name(id='remote_module', ctx=ast.Load())))

    def test_kernel_pattern_unsigned_byte_contract(self):
        value = control.pattern(0x0123456789abcdef, 7)
        for index in (0, 1, 511, 512, 65535, 65536, control.LENGTH-1):
            expected = ((0x0123456789abcdef >> ((index & 7)*8)) ^
                        (index*29) ^ (index >> 9) ^ (7*71)) & 255
            self.assertEqual(value[index], expected)

    def offline_fixture(self, root):
        source = root/'source.img'; source.write_bytes(b'owned source')
        scratch = root/'scratch.raw'; data = bytearray(control.CAPACITY)
        for number in range(8):
            start = 1024*1024+number*control.LENGTH
            data[start:start+control.LENGTH] = control.pattern(0xfeed1234, number)
        scratch.write_bytes(data)
        control.save(root/'inputs.json', {'source_image': str(source),
            'source_sha256': control.digest(source), 'scratch': str(scratch)})
        control.save(root/'runtime.json', {'passed': True, 'pid': 2147483647,
            'pid_starttime': 'missing', 'seed': 0xfeed1234})
        return source, scratch

    def test_offline_independent_payload_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); self.offline_fixture(root)
            control.verify(argparse.Namespace(output=root))
            self.assertTrue(json.loads((root/'result.json').read_text())['passed'])

    def test_offline_changed_nonce_outside_region_or_source_refuses(self):
        for kind in ('payload', 'outside', 'source'):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory); source, scratch = self.offline_fixture(root)
                path = source if kind == 'source' else scratch
                with path.open('r+b') as stream:
                    stream.seek(1024*1024 if kind == 'payload' else 0); stream.write(b'X')
                with self.assertRaises(ValueError): control.verify(argparse.Namespace(output=root))
                self.assertFalse((root/'result.json').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
