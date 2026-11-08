#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Prepare one pinned MIT-licensed action with its verified official release asset.

Only the browser-route download statement changes. The action's VM selection,
installation, synchronization, execution and acceptance remain upstream code.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile

ACTION_COMMIT = 'c46abacb49f09938ca4e1702d15d836285d694cc'
INDEX_SHA256 = 'c1e821ad07b8d5957a87298790de4bd50de9f6de87dcd6d91662ec943249d85f'
RUNTIME_VERSION = '0.7.2'
RUNTIME_SIZE = 618865
RUNTIME_SHA256 = '40a16e846bc07030759936d792343e770acdeb14d4c9991af7409648661a650d'
ASSET_API = 'repos/anyvm-org/anyvm/releases/assets/589232152'
CONFIG_SHA256 = '7979cdfa6380fb55b0d0de013a7c635a9a7ed2a89d2b17c6e6ead7e31a82ccb1'
LICENSE_SHA256 = 'df93e48cff786a5680c3c4832a6e35d61b71a932148a1dabfddbd745275495fb'
DOWNLOAD = b'await downloadFile(anyvmUrl, anyvmPath);'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_regular(path):
    if not stat.S_ISREG(path.lstat().st_mode):
        raise ValueError(f'Expected regular file: {path}')
    return path.read_bytes()


def git(directory, *args):
    return subprocess.check_output(['git', '-C', str(directory), *args], text=True).strip()


def replacement():
    # The same checks run again inside the action immediately before execution.
    return (f'if (anyvmVersion !== "{RUNTIME_VERSION}") {{\n'
            '        throw new Error("Preseeded AnyVM runtime version mismatch");\n'
            '      }\n'
            '      const preseededAnyvm = fs.readFileSync(anyvmPath);\n'
            f'      if (preseededAnyvm.length !== {RUNTIME_SIZE} ||\n'
            '          crypto.createHash("sha256").update(preseededAnyvm).digest("hex") !==\n'
            f'          "{RUNTIME_SHA256}") {{\n'
            '        throw new Error("Preseeded AnyVM runtime size/SHA256 mismatch");\n'
            '      }').encode()


def prepare(action, runtime, receipt):
    action = Path(action).resolve(strict=True)
    runtime = Path(runtime)
    receipt = Path(receipt)
    if Path(git(action, 'rev-parse', '--show-toplevel')).resolve() != action:
        raise ValueError('Action must be the root of its own checkout')
    if git(action, 'rev-parse', 'HEAD') != ACTION_COMMIT:
        raise ValueError('Action HEAD differs from pinned commit')
    if git(action, 'status', '--porcelain', '--untracked-files=no'):
        raise ValueError('Action tracked source is dirty; refusing preparation/repreparation')
    original = read_regular(action/'index.js')
    if digest(original) != INDEX_SHA256 or original.count(DOWNLOAD) != 1:
        raise ValueError('Pinned action index hash/download statement differs')
    config = read_regular(action/'conf/15.0.conf')
    if digest(config) != CONFIG_SHA256:
        raise ValueError('Pinned FreeBSD15.0 runtime/VM pair differs')
    license_data = read_regular(action/'LICENSE')
    if digest(license_data) != LICENSE_SHA256:
        raise ValueError('Pinned action MIT license differs')
    data = read_regular(runtime)
    if len(data) != RUNTIME_SIZE or digest(data) != RUNTIME_SHA256:
        raise ValueError('Official AnyVM runtime size/SHA256 mismatch')
    target = action/'anyvm.py'
    for path in (target, receipt):
        if os.path.lexists(path):
            raise ValueError(f'Refusing occupied output: {path}')
    # Preserve the original action line endings and every other source byte.
    newline = b'\r\n' if b'\r\n' in original else b'\n'
    patched = original.replace(DOWNLOAD, replacement().replace(b'\n', newline))
    result = {
        'schema': 'daemonarchy-freebsd-vm-bootstrap@1', 'status': 'prepared',
        'action_repository': 'vmactions/freebsd-vm', 'action_commit': ACTION_COMMIT,
        'index_original_sha256': INDEX_SHA256, 'index_prepared_sha256': digest(patched),
        'license_sha256': LICENSE_SHA256, 'config_sha256': CONFIG_SHA256,
        'runtime_version': RUNTIME_VERSION, 'runtime_asset_api': ASSET_API,
        'runtime_size': len(data), 'runtime_sha256': digest(data),
        'vm_release': '15.0', 'vm_arch': 'x86_64', 'builder_version': '2.2.8',
        'helper_sha256': digest(Path(__file__).read_bytes()),
    }
    receipt.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive outputs refuse stale/symlink destinations. Roll back our own
    # files if publication fails, retaining the untouched official checkout.
    runtime_created = receipt_created = index_changed = False
    temporary = None
    try:
        with target.open('xb') as stream:
            runtime_created = True
            stream.write(data)
        with receipt.open('xb') as stream:
            receipt_created = True
            stream.write((json.dumps(result, indent=2)+'\n').encode())
        with tempfile.NamedTemporaryFile(dir=action, prefix='.prepared-index-', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(patched)
        os.chmod(temporary, (action/'index.js').stat().st_mode & 0o777)
        os.replace(temporary, action/'index.js')
        index_changed = True
    except BaseException:
        if index_changed:
            (action/'index.js').write_bytes(original)
        if runtime_created:
            target.unlink()
        if receipt_created:
            receipt.unlink()
        raise
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--action-directory', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.action_directory, args.runtime, args.receipt)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Refused FreeBSD VM bootstrap: {error}\n')
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
