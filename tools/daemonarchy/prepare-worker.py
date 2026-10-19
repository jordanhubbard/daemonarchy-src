#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Explicit, optional worker bootstrap for native make input tracing.

Default inspection never changes the host. Enabling requires root and an exact
unprivileged build user; it neither installs packages nor changes boot settings.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import pwd
import stat
import subprocess

DEVICE = Path('/dev/filemon')
ENVIRONMENT = {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'LC_ALL': 'C', 'HOME': '/var/empty'}


def build_user(name):
    user = pwd.getpwnam(name)
    if user.pw_uid == 0 or user.pw_gid == 0:
        raise ValueError('filemon bootstrap requires an unprivileged build user and primary group')
    return user


def inspect(user):
    result = subprocess.run([
        '/usr/bin/make', '-r', '-f', '/dev/null', '-V', '.MAKE.PATH_FILEMON',
        '.MAKE.MODE=meta',
    ], env=ENVIRONMENT, capture_output=True, text=True, timeout=20, check=True)
    if result.stdout.strip() != str(DEVICE):
        raise ValueError('native make does not select the expected filemon device')
    try:
        info = DEVICE.lstat()
    except FileNotFoundError:
        return {'ready': False, 'reason': 'filemon-device-absent'}
    if not stat.S_ISCHR(info.st_mode):
        raise ValueError('filemon device is redirected or not a character device')
    ready = (info.st_uid == user.pw_uid and info.st_gid == user.pw_gid
             and stat.S_IMODE(info.st_mode) == 0o600)
    return {'ready': ready, 'reason': 'ready' if ready else 'filemon-device-owner-or-mode',
            'uid': info.st_uid, 'gid': info.st_gid, 'mode': oct(stat.S_IMODE(info.st_mode))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-user', required=True)
    parser.add_argument('--enable-filemon', action='store_true',
                        help='explicit root-only module load and owner-only device setup')
    args = parser.parse_args()
    if platform.system() != 'FreeBSD':
        raise ValueError('filemon worker bootstrap requires native FreeBSD')
    user = build_user(args.build_user)
    state = inspect(user)
    if args.enable_filemon and not state['ready']:
        if os.geteuid() != 0:
            raise ValueError('enabling filemon requires root; inspection made no changes')
        subprocess.run(['/sbin/kldload', '-n', 'filemon'], env=ENVIRONMENT,
                       capture_output=True, text=True, timeout=20, check=True)
        info = DEVICE.lstat()
        if not stat.S_ISCHR(info.st_mode):
            raise ValueError('filemon module did not expose a direct character device')
        os.chown(DEVICE, user.pw_uid, user.pw_gid, follow_symlinks=False)
        os.chmod(DEVICE, 0o600, follow_symlinks=False)
        state = inspect(user)
    print(json.dumps({'schema': 'daemonarchy/filemon-worker-readiness@1',
                      'build_user': user.pw_name, 'persistent_boot_change': False, **state}))
    return 0 if state['ready'] else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({'schema': 'daemonarchy/filemon-worker-readiness@1',
                          'ready': False, 'error': str(error)}))
        raise SystemExit(1)
