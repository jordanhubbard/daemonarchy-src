# SPDX-License-Identifier: BSD-2-Clause
"""Controller refusal tests; synthetic files are not native package acceptance."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("source_build", Path(__file__).with_name("build.py"))
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.parent = Path(self.temporary.name).resolve()
        self.source = self.parent / "src"
        self.source.mkdir()
        for name, content in {
            "tools/daemonarchy/src.conf": "WITHOUT_LOCAL_MODULES=yes\n",
            "sys/amd64/conf/GENERIC": "ident GENERIC\n",
            "sys/amd64/conf/DAEMONARCHY_DIAGNOSTIC": "include GENERIC\n",
            "sys/conf/newvers.sh": 'REVISION="16.0"\n',
        }.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        self.git("init", "-q")
        self.git("config", "user.email", "test@example.invalid")
        self.git("config", "user.name", "Test")
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")
        self.revision = self.git("rev-parse", "HEAD")
        self.git("update-ref", "refs/remotes/upstream/main", self.revision)
        source_patch = patch.object(build, "SOURCE", self.source)
        source_patch.start()
        self.addCleanup(source_patch.stop)
        self.output = self.parent / "output"

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.source), *args], text=True).strip()

    def plan(self, **kwargs):
        return build.make_plan(kwargs.get("revision", self.revision), kwargs.get("output", str(self.output)),
                               kwargs.get("profile", "baseline"), kwargs.get("jobs", 2),
                               kwargs.get("trace_mode", "off"))

    def built_fixture(self):
        plan = self.plan()
        self.output.mkdir()
        build.write_json(self.output / "plan.json", plan)
        entries = []
        for name in ("sets/base.txz", "sets/kernel.txz", "sets/lib32.txz", "sets/src.txz", "sets/MANIFEST",
                     "repo/FreeBSD-kernel-generic-16.pkg", "repo/FreeBSD-runtime-16.pkg",
                     "repo/FreeBSD-src-16.pkg", "repo/FreeBSD-src-sys-16.pkg"):
            path = self.output / name
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(b"synthetic verifier fixture, not a native package")
            entries.append({"path": name, "size": path.stat().st_size, "sha256": build.digest(path)})
        phases = []
        for phase in build.PHASES:
            log = self.output / (phase + ".log")
            log.write_text("synthetic phase\n")
            phases.append({"name": phase, "exit": 0, "log": log.name, "log_sha256": build.digest(log)})
        result = {"plan_identity": plan["identity"], "status": "built", "phases": phases, "artifacts": entries}
        build.write_json(self.output / "result.json", result)
        return plan, result

    def test_plan_binds_profile_and_does_not_create_output(self):
        baseline = self.plan()
        diagnostic = self.plan(profile="diagnostic")
        self.assertFalse(self.output.exists())
        self.assertNotEqual(baseline["identity"], diagnostic["identity"])
        self.assertEqual(baseline["upstream_base"], self.revision)
        self.assertIn("KERNCONF=DAEMONARCHY_DIAGNOSTIC", diagnostic["commands"]["buildkernel"])
        self.assertEqual(baseline["acceptance"], "not-qualified")

    def test_wrong_or_abbreviated_revision_refused(self):
        for revision in (self.revision[:12], "0" * 40, "main"):
            with self.subTest(revision=revision), self.assertRaises(ValueError):
                self.plan(revision=revision)

    def test_source_mutation_and_untracked_inputs_refused(self):
        path = self.source / "sys/conf/newvers.sh"
        path.write_text(path.read_text() + "# changed\n")
        with self.assertRaisesRegex(ValueError, "clean"):
            self.plan()
        self.git("checkout", "--", "sys/conf/newvers.sh")
        (self.source / "extra.conf").write_text("untracked")
        with self.assertRaisesRegex(ValueError, "clean"):
            self.plan()

    def test_unsafe_output_and_parallelism_refused(self):
        for output in ("relative", "/", str(self.source), str(self.parent), str(self.source / "obj"),
                       str(self.parent / "with space"), str(self.parent / "bad;path")):
            with self.subTest(output=output), self.assertRaises(ValueError):
                self.plan(output=output)
        for jobs in (0, -1, 257):
            with self.subTest(jobs=jobs), self.assertRaises(ValueError):
                self.plan(jobs=jobs)

    def test_native_gate_refuses_foreign_host(self):
        with patch.object(build.platform, "system", return_value="Linux"):
            with self.assertRaisesRegex(ValueError, "native"):
                build.native_preflight(self.parent, 1, 1)

    def test_actual_subprocess_failure_and_timeout(self):
        env = dict(os.environ)
        self.assertEqual(build.run_command([sys.executable, "-c", "raise SystemExit(7)"], env,
                                          self.parent / "failed.log", 10), 7)
        with self.assertRaises(subprocess.TimeoutExpired):
            build.run_command([sys.executable, "-c", "import time; time.sleep(60)"], env,
                              self.parent / "timeout.log", 0.1)

    def test_verifier_checks_bytes_without_claiming_acceptance(self):
        self.built_fixture()
        self.assertEqual(build.verify(self.output)["acceptance"], "not-qualified")
        (self.output / "sets/kernel.txz").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "changed"):
            build.verify(self.output)

    def test_changed_log_or_symlink_refused(self):
        self.built_fixture()
        path = self.output / "buildworld.log"
        path.write_text("changed")
        with self.assertRaisesRegex(ValueError, "changed"):
            build.verify(self.output)
        path.write_text("synthetic phase\n")
        path.unlink()
        path.symlink_to(self.output / "buildkernel.log")
        with self.assertRaisesRegex(ValueError, "changed"):
            build.verify(self.output)

    def test_missing_phase_or_artifact_inventory_refused(self):
        _, result = self.built_fixture()
        result["phases"].pop()
        build.write_json(self.output / "result.json", result)
        with self.assertRaisesRegex(ValueError, "phases"):
            build.verify(self.output)
        result["phases"].append({"name": "sets", "exit": 0, "log": "sets.log",
                                 "log_sha256": build.digest(self.output / "sets.log")})
        result["artifacts"] = result["artifacts"][:1]
        build.write_json(self.output / "result.json", result)
        with self.assertRaisesRegex(ValueError, "missing"):
            build.verify(self.output)

    def test_plan_tampering_and_failed_result_refused(self):
        plan, result = self.built_fixture()
        result["status"] = "failed"
        build.write_json(self.output / "result.json", result)
        with self.assertRaisesRegex(ValueError, "incomplete"):
            build.verify(self.output)
        plan["jobs"] = 25
        build.write_json(self.output / "plan.json", plan)
        with self.assertRaisesRegex(ValueError, "identity"):
            build.verify(self.output)

    def test_resume_cannot_rebind_an_existing_build(self):
        plan, _ = self.built_fixture()
        changed = dict(plan, jobs=3)
        args = type("Args", (), {"resume": True})()
        with self.assertRaisesRegex(ValueError, "resume plan differs"):
            build.run(changed, args)

    def test_trace_plan_is_explicit_and_preserves_untraced_plan_identity(self):
        plain = self.plan()
        self.assertNotIn("trace_mode", plain)
        self.assertEqual(plain, self.plan(trace_mode="off"))
        traced = self.plan(trace_mode="filemon")
        self.assertNotEqual(plain["identity"], traced["identity"])
        self.assertNotEqual(plain["package_version"], traced["package_version"])
        for command in traced["commands"].values():
            self.assertIn("UPDATE_DEPENDFILE=NO", command)
            self.assertIn("WITH_META_MODE=yes", command)
            self.assertTrue(any(a.startswith(".MAKE.MODE=") and "missing-filemon=yes" in a for a in command))
            self.assertFalse(any(a.startswith(".MAKE.MODE=") and "nofilemon" in a for a in command))
        with self.assertRaises(ValueError):
            self.plan(trace_mode="unknown")

    def trace_fixture(self):
        self.output.mkdir()
        path = self.output / "obj/unit/file.pieo.meta"
        path.parent.mkdir(parents=True)
        path.write_bytes(b"# Meta data file example\nCWD /fixture\nTARGET file.pieo\nCMD cc -c file.c\n"
                         b"\n-- filemon acquired metadata --\nV 5\nR 123 /fixture/file.c\n")
        (self.output / "obj/base.meta").write_text("./root type=dir mode=0755\n")
        return path

    def test_trace_snapshots_preserve_prior_bytes_across_later_phase_updates(self):
        path = self.trace_fixture()
        first = build.capture_trace_metadata(self.output, "buildworld")
        self.assertEqual(first["compiled_targets_with_filemon"], 1)
        self.assertEqual(len(first["metadata"]), 1)
        self.assertFalse(first["action_graph_complete"])
        self.assertEqual(first["acceptance"], "not-qualified")
        path.write_bytes(path.read_bytes() + b"R 123 /fixture/new-header.h\n")
        second = build.capture_trace_metadata(self.output, "buildkernel")
        self.assertNotEqual(first["metadata"][0]["sha256"], second["metadata"][0]["sha256"])
        build.verify_trace_metadata(self.output, [first, second], ["buildworld", "buildkernel"])
        blob = self.output / "trace-blobs" / first["metadata"][0]["sha256"]
        blob.chmod(0o644)
        blob.write_bytes(b"tampered historical observation")
        with self.assertRaisesRegex(ValueError, "changed"):
            build.verify_trace_metadata(self.output, [first, second], ["buildworld", "buildkernel"])

    def test_trace_without_real_filemon_compilation_is_refused(self):
        path = self.trace_fixture()
        path.write_bytes(b"# Meta data file example\nCMD cc -c file.c\n")
        with self.assertRaisesRegex(ValueError, "filemon"):
            build.capture_trace_metadata(self.output, "buildworld")
        path.write_bytes(b"# Meta data file example\n-- filemon acquired metadata --\n")
        with self.assertRaisesRegex(ValueError, "filemon"):
            build.capture_trace_metadata(self.output, "buildworld")
        path.unlink()
        with self.assertRaisesRegex(ValueError, "no observed compilation"):
            build.capture_trace_metadata(self.output, "buildworld")

    def test_trace_redirection_and_budgets_are_refused(self):
        path = self.trace_fixture()
        original = path.read_bytes()
        with patch.object(build, "TRACE_FILE_LIMIT", 10):
            with self.assertRaisesRegex(ValueError, "per-file"):
                build.capture_trace_metadata(self.output, "buildworld")
        with patch.object(build, "TRACE_TOTAL_LIMIT", 10):
            with self.assertRaisesRegex(ValueError, "aggregate"):
                build.capture_trace_metadata(self.output, "buildworld")
        path.unlink()
        other = self.parent / "outside.meta"
        other.write_bytes(original)
        path.symlink_to(other)
        with self.assertRaisesRegex(ValueError, "redirected"):
            build.capture_trace_metadata(self.output, "buildworld")

    def test_trace_phase_coverage_and_inventory_cannot_be_substituted(self):
        self.trace_fixture()
        observed = build.capture_trace_metadata(self.output, "buildworld")
        with self.assertRaisesRegex(ValueError, "cover"):
            build.verify_trace_metadata(self.output, [observed], ["buildworld", "buildkernel"])
        observed["metadata"][0]["path"] = "../outside"
        with self.assertRaisesRegex(ValueError, "inventory"):
            build.verify_trace_metadata(self.output, [observed], ["buildworld"])

    def test_trace_preflight_requires_existing_device_without_worker_mutation(self):
        with patch.object(build.platform, "system", return_value="FreeBSD"), \
                patch.object(build.platform, "machine", return_value="amd64"), \
                patch.object(build.os, "geteuid", return_value=1001), \
                patch.object(build.os, "access", return_value=False), \
                patch.object(build.subprocess, "check_output") as process:
            with self.assertRaisesRegex(ValueError, "already configured"):
                build.native_preflight(self.parent, 1, 1, "filemon")
            process.assert_not_called()


if __name__ == "__main__":
    unittest.main()
