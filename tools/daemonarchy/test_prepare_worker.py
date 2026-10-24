# SPDX-License-Identifier: BSD-2-Clause
"""Worker feature refusal checks; native loading is qualified separately."""
import importlib.util
from pathlib import Path
import stat
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('prepare_worker', Path(__file__).with_name('prepare-worker.py'))
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.user = SimpleNamespace(pw_name='builder', pw_uid=1001, pw_gid=1001)

    def test_privileged_user_or_primary_group_refuses(self):
        for uid, gid in ((0, 1001), (1001, 0)):
            with patch.object(worker.pwd, 'getpwnam', return_value=SimpleNamespace(pw_uid=uid, pw_gid=gid)):
                with self.assertRaisesRegex(ValueError, 'unprivileged'):
                    worker.build_user('builder')

    def test_absent_device_reports_unready_without_loading(self):
        with patch.object(worker.subprocess, 'run', return_value=SimpleNamespace(stdout='/dev/filemon\n')) as run:
            with patch.object(Path, 'lstat', side_effect=FileNotFoundError):
                self.assertFalse(worker.inspect(self.user)['ready'])
            self.assertEqual(run.call_count, 1)
            self.assertEqual(run.call_args.kwargs['env'], worker.ENVIRONMENT)
            self.assertEqual(run.call_args.args[0][0], '/usr/bin/make')

    def test_wrong_make_provider_refuses_before_device_access(self):
        with patch.object(worker.subprocess, 'run', return_value=SimpleNamespace(stdout='/tmp/other\n')):
            with patch.object(Path, 'lstat') as read:
                with self.assertRaisesRegex(ValueError, 'expected filemon'):
                    worker.inspect(self.user)
                read.assert_not_called()

    def test_direct_character_device_requires_exact_owner_only_permissions(self):
        with patch.object(worker.subprocess, 'run', return_value=SimpleNamespace(stdout='/dev/filemon\n')):
            for mode, ready in ((stat.S_IFCHR | 0o600, True), (stat.S_IFCHR | 0o666, False)):
                info = SimpleNamespace(st_mode=mode, st_uid=1001, st_gid=1001)
                with patch.object(Path, 'lstat', return_value=info):
                    self.assertEqual(worker.inspect(self.user)['ready'], ready)
            with patch.object(Path, 'lstat', return_value=SimpleNamespace(st_mode=stat.S_IFLNK | 0o777)):
                with self.assertRaisesRegex(ValueError, 'redirected'):
                    worker.inspect(self.user)

    def test_unprivileged_enable_refuses_before_module_load(self):
        with patch('sys.argv', ['prepare-worker.py', '--build-user', 'builder', '--enable-filemon']), \
                patch.object(worker.platform, 'system', return_value='FreeBSD'), \
                patch.object(worker, 'build_user', return_value=self.user), \
                patch.object(worker, 'inspect', return_value={'ready': False}), \
                patch.object(worker.os, 'geteuid', return_value=1001), \
                patch.object(worker.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'requires root'):
                worker.main()
            run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
