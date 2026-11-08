#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Private-VM control for the diagnostic native translated busdma fixture.

This does not start, stop or modify a VM's boot configuration. The parent runner
owns lifecycle and must boot native SD root +GENET with only the fresh16MiB USB
scratch file produced here. Only loopback SSH with a pinned host key is accepted.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import socket
import struct
import subprocess
import xml.etree.ElementTree as ET

LENGTH = 128 * 1024
CAPACITY = 16 * 1024 * 1024
OFFSET = 1 << 36
MODEL_SHA256 = '08d5088766b2670847f0ec1c57cc7aeceb24f6c3fc98caff6ed2f6265718d834'
KERNEL_SHA256 = 'da51c55c69c3ab171d391cf49e6273de8d120885cbbaf5097984888825b55629'
MODULE_SHA256 = 'e313842268d53a9194c3f20c9fe267dda843b69480602bdf12e59c8aa01e3342'
MAXCOMLEN = 19  # Frozen sys/sys/param.h; paired with the pinned native kernel.
INTRNAME_STRIDE = 2 * MAXCOMLEN + 1
PRIVATE_LIMIT = 0x1fffffff


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n')


def zones(text):
    result = {}
    for line in text.splitlines():
        match = re.fullmatch(r'hw\.busdma\.(zone\d+)\.([a-z_]+):\s*(\S+)', line)
        if match:
            name, field, value = match.groups()
            if field in {'lowaddr', 'alignment', 'total_bpages', 'free_bpages', 'reserved_bpages', 'active_bpages', 'total_deferred', 'total_bounced', 'domain'}:
                result.setdefault(name, {})[field] = int(value, 0)
    if not result:
        raise ValueError('No actual native bounce-zone observations')
    for name, value in result.items():
        for field in ('lowaddr', 'alignment', 'total_bpages', 'free_bpages',
                      'reserved_bpages', 'active_bpages', 'total_deferred'):
            if field not in value:
                raise ValueError(f'Incomplete native zone {name}: {field}')
    return result


def isolated_zone(before, after, page_size):
    # alloc_bounce_zone can reuse any stricter zone, not just equal limits.
    if any(v['lowaddr'] <= PRIVATE_LIMIT and v['alignment'] >= page_size
           for v in before.values()):
        raise ValueError('An existing stricter pool could be shared; refusing pressure')
    added = set(after)-set(before)
    if len(added) != 1:
        raise ValueError(f'Expected one observed new private pool, found {added}')
    name = added.pop()
    value = after[name]
    if value['lowaddr'] != PRIVATE_LIMIT or value['alignment'] != page_size:
        raise ValueError('Actual private pool constraints differ')
    if not 1 <= value['total_bpages'] <= 8192:
        raise ValueError('Actual pool allocation exceeds bounded pressure budget')
    check_free(value)
    return name


def check_free(value):
    if (value['free_bpages'] != value['total_bpages'] or
            value['reserved_bpages'] != 0 or value['active_bpages'] != 0):
        raise ValueError('Native pool retains active/reserved/consumed pages')


def interrupts(names, counts):
    # Pinned ARM64 ABI: u_long is 64-bit little endian. subr_intr.c's
    # INTRNAME_LEN = 2*MAXCOMLEN+1, with MAXCOMLEN 19 in sys/param.h.
    if not counts or len(counts) % 8 or len(counts) // 8 > 65536:
        raise ValueError('Invalid bounded ARM64 interrupt-count vector')
    slots = len(counts) // 8
    if len(names) % slots or len(names) // slots != INTRNAME_STRIDE:
        raise ValueError('Interrupt-name stride differs from pinned39-byte ABI')
    result = []
    for index, count in enumerate(struct.unpack('<'+'Q'*slots, counts)):
        raw = names[index*INTRNAME_STRIDE:(index+1)*INTRNAME_STRIDE]
        if raw == bytes(INTRNAME_STRIDE):
            name = ''  # Unused slots are not delimiters between active names.
        else:
            if raw[-1] != 0 or any(value < 32 or value > 126 for value in raw[:-1]):
                raise ValueError('Malformed padded native interrupt name')
            name = raw[:-1].decode('ascii').rstrip(' ')
        result.append({'index': index, 'name': name, 'count': count})
    return result


def xhci_interrupt_progress(before, after):
    choose = lambda rows: [row for row in rows if not row['name'].startswith('stray ') and re.search(r'(?:^|[ :])xhci0(?:$|[ ,])', row['name'])]
    first, last = choose(before), choose(after)
    if len(first) != 1 or len(last) != 1:
        raise ValueError('Expected exactly one actual native xhci0 IRQ slot')
    old, new = first[0], last[0]
    if len(before) != len(after) or old['index'] != new['index'] or old['name'] != new['name']:
        raise ValueError('Actual xhci0 IRQ identity changed')
    if new['count'] <= old['count']:
        raise ValueError('Actual xhci0 interrupt counter did not increase')
    stray_index = old['index']+1
    if stray_index >= len(before):
        raise ValueError('Missing paired stray IRQ slot')
    old_stray, new_stray = before[stray_index], after[stray_index]
    if (old_stray['name'] != 'stray '+old['name'] or
            new_stray['name'] != old_stray['name'] or
            new_stray['count'] < old_stray['count']):
        raise ValueError('Paired stray IRQ identity/counter differs')
    return {'index': old['index'], 'name': old['name'], 'before': old['count'],
            'after': new['count'], 'delta': new['count']-old['count'],
            'stray_before': old_stray['count'], 'stray_after': new_stray['count'],
            'stray_delta': new_stray['count']-old_stray['count']}


def pattern(seed, round_number):
    return bytes(((seed >> ((i & 7)*8)) ^ (i*29) ^ (i >> 9) ^
                  (round_number*71)) & 255 for i in range(LENGTH))


# GEOM traversal adapted from daemonarchy d2615d6b24d5 (BSD-2-Clause),
# tools/hwtrace/pi4-usb-network-simulator.py; retain strict actual DISK ancestry.
def root_disk(xml, name):
    """Resolve a mounted GEOM provider through consumers to actual DISK providers."""
    if len(xml) > 1 << 20:
        return set()
    try:
        # kern.geom.confxml uses SBUF_INCLUDENUL; sysctl -b preserves that byte.
        tree = ET.fromstring(xml.removesuffix("\0"))
    except ET.ParseError:
        return set()
    providers, names = {}, {}
    for cls in tree.findall('class'):
        kind = cls.findtext('name')
        for geom in cls.findall('geom'):
            parents = [p.get('ref') for p in geom.findall('consumer/provider')]
            for provider in geom.findall('provider'):
                ident, label = provider.get('id'), provider.findtext('name')
                if not ident or not label or ident in providers:
                    return set()
                providers[ident] = (kind, label, parents)
                names.setdefault(label, []).append(ident)
    starts = names.get(name.removeprefix('/dev/'), [])
    if len(starts) != 1:
        return set()
    visited, disks = set(), set()
    def walk(ident):
        if ident in visited or ident not in providers:
            return False
        visited.add(ident)
        kind, label, parents = providers[ident]
        if kind == 'DISK':
            disks.add(label)
            return True
        return bool(parents) and all(walk(parent) for parent in parents)
    return disks if walk(starts[0]) else set()



def usb_scratch_bound(qtree):
    lines = qtree.splitlines()
    for index, line in enumerate(lines):
        if line.strip() != 'bus: bcm-xhci.0':
            continue
        indentation = len(line)-len(line.lstrip())
        branch = []
        for value in lines[index+1:]:
            if value.strip() and len(value)-len(value.lstrip()) <= indentation:
                break
            branch.append(value)
        for start, value in enumerate(branch):
            if not value.strip().startswith('dev: usb-storage,'):
                continue
            device_indent = len(value)-len(value.lstrip())
            for item in branch[start+1:]:
                if item.strip() and len(item)-len(item.lstrip()) <= device_indent:
                    break
                if item.strip() == 'drive = "dmadisk"':
                    return True
    return False


def validate_blocks(blocks, inputs, sd_overlay):
    inserted = [item for item in blocks if 'inserted' in item]
    if len(inserted) != 2:
        raise ValueError('Only the actual SD overlay and USB scratch may be attached')
    scratch = [item for item in inserted if item.get('device') == 'dmadisk']
    if len(scratch) != 1:
        raise ValueError('Missing explicit dmadisk backend')
    usb = scratch[0]['inserted']
    if (Path(usb['file']).resolve() != Path(inputs['scratch']).resolve() or
            usb['image']['virtual-size'] != CAPACITY or usb['image']['format'] != 'raw'):
        raise ValueError('Live private scratch backend differs')
    sd = next(item['inserted'] for item in inserted if item is not scratch[0])
    image = sd['image']
    if Path(sd['file']).resolve() != sd_overlay.resolve() or image['format'] != 'qcow2':
        raise ValueError('Live SD overlay differs')
    backing = image.get('full-backing-filename') or image.get('backing-filename')
    if not backing:
        raise ValueError('No actual SD backing custody')
    path = Path(backing)
    if not path.is_absolute(): path = sd_overlay.parent/path
    if path.resolve() != Path(inputs['source_image']).resolve():
        raise ValueError('SD backing source differs')


class QMP:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(10)
        self.socket.connect(str(path))
        self.stream = self.socket.makefile('rwb')
        greeting = json.loads(self.stream.readline())
        if 'QMP' not in greeting:
            raise ValueError('No genuine QMP greeting')
        self.command('qmp_capabilities')

    def command(self, name, arguments=None):
        self.stream.write((json.dumps({'execute': name, 'arguments': arguments or {}})+'\n').encode())
        self.stream.flush()
        while True:
            value = json.loads(self.stream.readline())
            if 'error' in value:
                raise ValueError(value)
            if 'return' in value:
                return value['return']

    def close(self):
        self.stream.close()
        self.socket.close()


def prepare(args):
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.source_image.resolve(strict=True)
    scratch = args.output/'scratch.raw'
    with scratch.open('xb') as stream:
        stream.truncate(CAPACITY)
    save(args.output/'inputs.json', {'source_image': str(source),
         'source_sha256': digest(source), 'scratch': str(scratch.resolve()),
         'scratch_size': CAPACITY, 'scratch_initial_sha256': digest(scratch),
         'scope': 'private VM only; no physical writes'})
    print(scratch.resolve())


def run(args):
    root = args.output.resolve(strict=True)
    if (root/'runtime.json').exists():
        raise ValueError('Refusing occupied runtime evidence')
    inputs = json.loads((root/'inputs.json').read_text())
    results = {'passed': False, 'commands': [], 'scope': 'native diagnostic backend/USB fixture, not BCM2712 hardware or MSI acceptance'}
    def record(argv, timeout=120):
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        results['commands'].append({'argv': argv, 'exit': result.returncode,
                                    'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise ValueError(f'Native command failed: {argv[-1]}: {result.stderr}')
        return result.stdout.strip()
    connection = ['-F', '/dev/null', '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes',
                  '-o', 'IdentityAgent=none', '-o', 'ForwardAgent=no',
                  '-o', 'GlobalKnownHostsFile=/dev/null', '-o', 'ControlMaster=no',
                  '-o', 'ConnectTimeout=3', '-o', 'ConnectionAttempts=1',
                  '-o', 'StrictHostKeyChecking=yes',
                  '-o', f'UserKnownHostsFile={args.known_hosts.resolve()}',
                  '-i', str(args.identity.resolve())]
    ssh = ['ssh', *connection, '-p', str(args.ssh_port), 'root@127.0.0.1']
    def remote(*argv, timeout=120):
        nonce = secrets.token_hex(12)
        begin, end = 'DMA_BEGIN_'+nonce, 'DMA_END_'+nonce
        script = ("set -eu; printf '%s\\n' "+shlex.quote(begin)+'; '+shlex.join(argv)+
                  "; printf '\\n%s\\n' "+shlex.quote(end))
        raw = record(ssh+[script], timeout)
        if raw.count(begin) != 1 or raw.count(end) != 1:
            raise ValueError('Missing/duplicate authenticated command framing')
        first, last = raw.find(begin+'\n'), raw.rfind('\n'+end)
        if first < 0 or last < first+len(begin)+1:
            raise ValueError('Malformed authenticated command framing')
        return raw[first+len(begin)+1:last].strip()
    def transfer(source, target):
        return record(['scp', *connection, '-P', str(args.ssh_port), source, target])
    def interrupt_snapshot(stage):
        remote_names = '/var/tmp/dma-intrnames-'+stage+'.bin'
        remote_counts = '/var/tmp/dma-intrcnt-'+stage+'.bin'
        script = ('sysctl -b hw.intrnames > '+shlex.quote(remote_names)+
                  ' && sysctl -b hw.intrcnt > '+shlex.quote(remote_counts))
        remote('/rescue/rescue', 'sh', '-c', script)
        names, counts = root/('intrnames-'+stage+'.bin'), root/('intrcnt-'+stage+'.bin')
        transfer('root@127.0.0.1:'+remote_names, str(names))
        transfer('root@127.0.0.1:'+remote_counts, str(counts))
        return {'records': interrupts(names.read_bytes(), counts.read_bytes()),
                'names_sha256': digest(names), 'counts_sha256': digest(counts)}
    def sysctl(name):
        return remote('sysctl', '-n', name)
    def snapshot():
        return zones(remote('sysctl', 'hw.busdma'))
    def operation(number):
        remote('sysctl', f'debug.bcm2711_dma_probe.run={number}', timeout=180)
        text = sysctl('debug.bcm2711_dma_probe.status')
        if not text.startswith('PASS' if number in (1, 4, 6) else 'PREPARED'):
            raise ValueError(text)
        return text
    native_ready = False
    try:
        if digest(args.qemu) != MODEL_SHA256 or digest(args.kernel) != KERNEL_SHA256:
            raise ValueError('Qualified model/kernel hash differs from frozen fixture')
        executable = Path(f'/proc/{args.pid}/exe').resolve(strict=True)
        if digest(executable) != MODEL_SHA256:
            raise ValueError('Live process does not execute the qualified QEMU binary')
        results['pid'] = args.pid
        results['pid_starttime'] = Path(f'/proc/{args.pid}/stat').read_text().split()[21]
        results['model_sha256'], results['kernel_sha256'] = MODEL_SHA256, KERNEL_SHA256
        results['module_sha256'] = digest(args.module)
        if results['module_sha256'] != MODULE_SHA256:
            raise ValueError('Module differs from allocated native e500 build')
        results['controller_sha256'] = digest(__file__)
        qmp = QMP(args.qmp)
        try:
            results['qmp_status'] = qmp.command('query-status')
            if results['qmp_status']['status'] != 'running':
                raise ValueError('Guest is not running')
            blocks = qmp.command('query-block')
            qtree = qmp.command('human-monitor-command', {'command-line': 'info qtree'})
            results['qmp_blocks'], results['qmp_qtree'] = blocks, qtree
            validate_blocks(blocks, inputs, args.sd_overlay.resolve(strict=True))
            if not usb_scratch_bound(qtree):
                raise ValueError('Real modeled USB storage is not present')
        finally:
            qmp.close()
        if sysctl('kern.ident') != 'BCM2711_DMA_TEST':
            raise ValueError('Wrong running kernel configuration')
        if int(sysctl('hw.ncpu')) != 4 or int(sysctl('hw.physmem')) < 7 * (1 << 30):
            raise ValueError('Full8GiB/four-CPU profile required for genuine high pages')
        bootfile = sysctl('kern.bootfile')
        if not bootfile.startswith('/') or any(ord(char) < 32 for char in bootfile):
            raise ValueError('Guest bootfile is not one canonical absolute path')
        guest_kernel = root/'guest-kernel'
        transfer('root@127.0.0.1:'+bootfile, str(guest_kernel))
        if digest(guest_kernel) != KERNEL_SHA256:
            raise ValueError('Actual guest bootfile read back through SCP differs')
        results['guest_kernel_roundtrip_sha256'] = digest(guest_kernel)
        mounts = remote('mount', '-p')
        root_mount = [line.split() for line in mounts.splitlines() if len(line.split()) > 2 and line.split()[1] == '/']
        results['geom_xml'] = remote('sysctl', '-b', 'kern.geom.confxml')
        if len(root_mount) != 1 or root_mount[0][2] != 'ufs' or root_disk(results['geom_xml'], root_mount[0][0]) != {'mmcsd0'}:
            raise ValueError('Actual mounted GEOM ancestry must reach SD DISK mmcsd0 only')
        results['root_mount'] = root_mount[0]
        results['genet'] = remote('ifconfig', 'genet0')
        if not re.search(r'status:\s+active', results['genet']) or not re.search(r'inet\s+10\.0\.2\.15\b', results['genet']):
            raise ValueError('Actual GENET carrier/DHCP address missing')
        for key, value in {'ready': 1, 'window_low': 0x12, 'window_high': 0x10,
                           'ring_mask': 7, 'ring_errors': 0}.items():
            if int(sysctl('hw.bcm2711_dma_test.'+key), 0) != value:
                raise ValueError('Cold hardware/tag/ring pairing is not ready: '+key)
        for key in ('hw.pci.enable_msi', 'hw.pci.enable_msix'):
            if int(sysctl(key)) != 0:
                raise ValueError('MSI must remain explicitly disabled in this INTx fixture')
        native_ready = True
        results['rings'] = {key: int(sysctl('hw.bcm2711_dma_test.'+key), 0)
                            for key in ('dcbaap', 'erstba', 'crcr')}
        if any(value < OFFSET or value-OFFSET >= 0x3c001000 for value in results['rings'].values()):
            raise ValueError('Actual programmed rings are not translated')
        remote_module = '/var/tmp/bcm2711_dma_probe-'+results['module_sha256']+'.ko'
        transfer(str(args.module.resolve()), 'root@127.0.0.1:'+remote_module)
        module_roundtrip = root/'guest-module.ko'
        transfer('root@127.0.0.1:'+remote_module, str(module_roundtrip))
        if digest(module_roundtrip) != results['module_sha256']:
            raise ValueError('Actual guest module SCP roundtrip differs')
        results['guest_module_roundtrip_sha256'] = digest(module_roundtrip)
        remote('/rescue/rescue', 'kldload', remote_module)
        results['interrupts_before'] = interrupt_snapshot('before')
        results['high_before'] = operation(1)
        page_size = int(sysctl('hw.pagesize'))
        before = snapshot()
        # Refuse eligible shared pools BEFORE allocating any pressure maps.
        if any(v['lowaddr'] <= PRIVATE_LIMIT and v['alignment'] >= page_size for v in before.values()):
            raise ValueError('Existing stricter pool would make pressure unsafe')
        results['pressure_prepared'] = operation(2)
        prepared = snapshot()
        name = isolated_zone(before, prepared, page_size)
        remote('sysctl', 'debug.bcm2711_dma_probe.pool_pages='+str(prepared[name]['total_bpages']))
        results['pressure'] = operation(4)
        after = snapshot()
        check_free(after[name])
        if (after[name]['total_bpages'] != prepared[name]['total_bpages'] or
                after[name]['total_deferred'] != prepared[name]['total_deferred']+1):
            raise ValueError('Actual pressure pool allocation/deferred accounting differs')
        results['error_prepared'] = operation(5)
        error_before = snapshot()
        check_free(error_before[name])
        results['errors'] = operation(6)
        error_after = snapshot()
        for field in ('total_bpages', 'free_bpages', 'reserved_bpages', 'active_bpages'):
            if error_after[name][field] != error_before[name][field]:
                raise ValueError('EFBIG partial unwind changed private pool '+field)
        results['high_after'] = operation(1)
        match = re.search(r'seed=(0x[0-9a-f]+|0)$', results['high_after'])
        if not match:
            raise ValueError('Missing fresh recovery nonce')
        results['seed'] = int(match[1], 0)
        results['zones'] = {'before': before, 'prepared': prepared, 'after_pressure': after,
                            'before_errors': error_before, 'after_errors': error_after, 'private': name}
        results['dmesg'] = remote('dmesg')
        if len(re.findall(r'BCM_DMA_TEST (?:READ|WRITE) round=', results['dmesg'])) < 32:
            raise ValueError('Missing actual high-buffer USB completion witnesses')
        if re.search(r'CAM status:|g_vfs_done\(.*error|panic:', results['dmesg']):
            raise ValueError('Native storage/owner error in kernel log')
        results['interrupts_after'] = interrupt_snapshot('after')
        results['interrupt_progress'] = xhci_interrupt_progress(results['interrupts_before']['records'], results['interrupts_after']['records'])
        remote('/rescue/rescue', 'kldunload', 'bcm2711_dma_probe')
        remote('/rescue/rescue', 'sync')
        results['passed'] = True
    except BaseException as error:
        results['error'] = repr(error)
        if native_ready:
            for command in (('sysctl', '-n', 'debug.bcm2711_dma_probe.status'), ('dmesg',)):
                try:
                    remote(*command)
                except Exception:
                    pass
        raise
    finally:
        save(root/'runtime.json', results)
    print(json.dumps({'passed': True, 'stage': 'runtime', 'evidence': str(root/'runtime.json')}))


def verify(args):
    root = args.output.resolve(strict=True)
    if (root/'result.json').exists():
        raise ValueError('Refusing occupied offline evidence')
    inputs = json.loads((root/'inputs.json').read_text())
    runtime = json.loads((root/'runtime.json').read_text())
    if not runtime['passed']:
        raise ValueError('Runtime controls did not pass')
    proc = Path(f"/proc/{runtime['pid']}/stat")
    if proc.exists() and proc.read_text().split()[21] == runtime['pid_starttime']:
        raise ValueError('Guest process still exists; offline lifecycle proof pending')
    if digest(inputs['source_image']) != inputs['source_sha256']:
        raise ValueError('Original SD source image changed')
    data = Path(inputs['scratch']).read_bytes()
    if len(data) != CAPACITY or any(data[:1024*1024]) or any(data[2*1024*1024:]):
        raise ValueError('Scratch geometry/outside-range preservation failed')
    for number in range(8):
        start = 1024*1024+number*LENGTH
        if data[start:start+LENGTH] != pattern(runtime['seed'], number):
            raise ValueError('Independent offline USB nonce mismatch '+str(number))
    result = {'passed': True, 'stage': 'offline', 'scratch_sha256': digest(inputs['scratch']),
              'runtime_sha256': digest(root/'runtime.json'), 'source_unchanged': True,
              'actual_usb_payload_bytes_each_direction_per_run': 1024*1024,
              'high_bio_runs': 2, 'production_hardware_acceptance': False,
              'natural_exit_and_diagnostics': 'require separate parent runner receipt'}
    save(root/'result.json', result)
    print(json.dumps(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    prepare_parser = sub.add_parser('prepare')
    prepare_parser.add_argument('--source-image', type=Path, required=True)
    run_parser = sub.add_parser('run')
    for name in ('qemu', 'kernel', 'module', 'qmp', 'identity', 'known-hosts', 'sd-overlay'):
        run_parser.add_argument('--'+name, type=Path, required=True)
    run_parser.add_argument('--pid', type=int, required=True)
    run_parser.add_argument('--ssh-port', type=int, required=True)
    sub.add_parser('verify')
    for selected in sub.choices.values():
        selected.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    {'prepare': prepare, 'run': run, 'verify': verify}[args.command](args)


if __name__ == '__main__':
    main()
