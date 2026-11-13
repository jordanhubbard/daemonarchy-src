# SPDX-License-Identifier: BSD-2-Clause
"""Controller refusal tests; synthetic files are not native package acceptance."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tarfile
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
            "sys/arm64/conf/GENERIC": "ident GENERIC\n",
            "sys/conf/newvers.sh": 'REVISION="16.0"\nBRANCH="CURRENT"\n',
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
                               kwargs.get("trace_mode", "off"), kwargs.get("target", "amd64"))

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
        manifest = self.output / "sets/MANIFEST"
        manifest.write_text("".join(f'{Path(e["path"]).name}\t{e["sha256"]}\t1\tfixture\t"synthetic"\toff\n'
                                    for e in entries if e["path"].endswith(".txz")))
        next(e for e in entries if e["path"] == "sets/MANIFEST").update(
            size=manifest.stat().st_size, sha256=build.digest(manifest))
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

    def test_arm_plan_binds_native_target_config_and_every_phase(self):
        arm = self.plan(target="arm64")
        amd = self.plan()
        self.assertEqual(amd, self.plan(target="amd64"))
        self.assertNotEqual(arm["identity"], amd["identity"])
        self.assertEqual((arm["target"], arm["target_arch"]), ("arm64", "aarch64"))
        self.assertIn("sys/arm64/conf/GENERIC", arm["configurations"])
        self.assertNotIn("sys/amd64/conf/GENERIC", arm["configurations"])
        for argv in [*arm["commands"].values(), arm["release_directory_query"]]:
            self.assertIn("TARGET=arm64", argv)
            self.assertIn("TARGET_ARCH=aarch64", argv)
            self.assertNotIn("TARGET=amd64", argv)
            self.assertIn("NO_ROOT=yes", argv)
        self.assertFalse(self.output.exists())

    def test_unsupported_target_or_missing_arm_profile_cannot_fall_back(self):
        with self.assertRaisesRegex(ValueError, "unsupported native target"):
            self.plan(target="riscv64")
        with self.assertRaisesRegex(ValueError, "arm64 kernel profile DAEMONARCHY_DIAGNOSTIC"):
            self.plan(target="arm64", profile="diagnostic")

    def test_native_target_matching_cannot_admit_cross_builds(self):
        for target, machine, matches in (("amd64", "amd64", True),
                ("amd64", "x86_64", True), ("amd64", "arm64", False),
                ("arm64", "arm64", True), ("arm64", "aarch64", True),
                ("arm64", "amd64", False), ("arm64", "riscv64", False)):
            with self.subTest(target=target, machine=machine), \
                    patch.object(build.platform, "system", return_value="FreeBSD"), \
                    patch.object(build.platform, "machine", return_value=machine), \
                    patch.object(build.platform, "release", return_value="16.0-CURRENT"), \
                    patch.object(build.os, "geteuid", return_value=1001), \
                    patch.object(build.os, "access", return_value=False) as access:
                message = "required installed tool" if matches else f"native {target} FreeBSD"
                with self.assertRaisesRegex(ValueError, message):
                    build.native_preflight(self.parent, 1, 1, target=target)
                if not matches:
                    access.assert_not_called()

    def test_native_gate_refuses_wrong_release_before_tools_or_builds(self):
        for target, machine in (("amd64", "amd64"), ("arm64", "arm64")):
            for release in ("15.0-RELEASE", "16.0-RELEASE", "16.0-STABLE",
                            "17.0-CURRENT", "unknown", "16.0-CURRENT-invalid"):
                with self.subTest(target=target, release=release), \
                        patch.object(build.platform, "system", return_value="FreeBSD"), \
                        patch.object(build.platform, "machine", return_value=machine), \
                        patch.object(build.platform, "release", return_value=release), \
                        patch.object(build.os, "geteuid", return_value=1001), \
                        patch.object(build.os, "access") as access, \
                        patch.object(build.subprocess, "Popen") as execute:
                    with self.assertRaisesRegex(ValueError, "native 16.0-CURRENT"):
                        build.native_preflight(self.parent, 1, 80, target=target)
                    access.assert_not_called()
                    execute.assert_not_called()
                    self.assertFalse(self.output.exists())

    def test_matching_current_release_still_checks_all_worker_resources(self):
        for target, machine in (("amd64", "amd64"), ("arm64", "arm64")):
            with self.subTest(target=target), \
                    patch.object(build.platform, "system", return_value="FreeBSD"), \
                    patch.object(build.platform, "machine", return_value=machine), \
                    patch.object(build.platform, "release", return_value="16.0-CURRENT"), \
                    patch.object(build.os, "geteuid", return_value=1001), \
                    patch.object(build.os, "access", return_value=True) as access, \
                    patch.object(build.os, "cpu_count", return_value=4), \
                    patch.object(build.shutil, "disk_usage") as disk, \
                    patch.object(build.subprocess, "check_output",
                                 side_effect=["2097152", "0", "4096"]) as probe, \
                    patch.object(build.subprocess, "Popen") as execute:
                disk.return_value.free = 100 * 1024**3
                build.native_preflight(self.parent, 4, 80, target=target)
                self.assertEqual(access.call_count, 3)
                self.assertEqual(disk.call_count, 2)
                self.assertEqual(probe.call_count, 3)
                execute.assert_not_called()
                self.assertFalse(self.output.exists())

    def test_noncurrent_source_is_not_admitted_as_current(self):
        (self.source / "sys/conf/newvers.sh").write_text('REVISION="16.0"\nBRANCH="STABLE"\n')
        with patch.object(build.platform, "system", return_value="FreeBSD"), \
                patch.object(build.platform, "machine", return_value="amd64"), \
                patch.object(build.os, "geteuid", return_value=1001), \
                patch.object(build.os, "access") as access:
            with self.assertRaisesRegex(ValueError, "source must declare a CURRENT"):
                build.native_preflight(self.parent, 1, 80)
            access.assert_not_called()

    def test_arm_native_gate_keeps_unprivileged_requirement(self):
        with patch.object(build.platform, "system", return_value="FreeBSD"), \
                patch.object(build.platform, "machine", return_value="arm64"), \
                patch.object(build.os, "geteuid", return_value=0):
            with self.assertRaisesRegex(ValueError, "unprivileged"):
                build.native_preflight(self.parent, 1, 1, target="arm64")

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

    def test_resume_preserves_unrecorded_crash_logs_and_occupied_names(self):
        plan, state = self.built_fixture()
        logs = self.output / "logs"
        logs.mkdir()
        previous = []
        for attempt in (1, 2):
            path = logs / f"buildworld-{attempt}.log"
            path.write_text("previous controlled failed attempt\n")
            previous.append({"name": "buildworld", "exit": 7,
                             "log": str(path.relative_to(self.output)),
                             "log_sha256": build.digest(path)})
        state.update(status="running", active_phase="buildworld", phases=previous)
        build.write_json(self.output / "result.json", state)
        orphan = logs / "buildworld-3.log"
        orphan.write_bytes(b"interrupted phase output\n")
        missing = self.parent / "missing-log-target"
        occupied = logs / "buildworld-4.log"
        occupied.symlink_to(missing)
        (logs / "buildkernel-99.log").write_text("unrelated phase\n")
        original_command = build.run_command

        def controlled_command(argv, env, log, timeout):
            return original_command(
                [sys.executable, "-c",
                 "print('synthetic resumed phase'); raise SystemExit(7)"],
                env, log, timeout)

        args = type("Args", (), {"resume": True, "jobs": 2,
                                 "minimum_free_gib": 0, "phase_timeout": 10})()
        with patch.object(build, "native_preflight"), \
                patch.object(build, "run_command", side_effect=controlled_command):
            with self.assertRaisesRegex(ValueError, "buildworld failed"):
                build.run(plan, args)
        self.assertEqual(orphan.read_bytes(), b"interrupted phase output\n")
        self.assertTrue(occupied.is_symlink())
        self.assertFalse(missing.exists())
        result = json.loads((self.output / "result.json").read_text())
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["phases"][-1]["exit"], 7)
        self.assertEqual(result["phases"][-1]["log"], "logs/buildworld-5.log")
        self.assertIn("synthetic resumed phase",
                      (logs / "buildworld-5.log").read_text())

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
            self.assertFalse(any(a.startswith(".MAKE.MODE=") and "verbose" in a.split() for a in command))
        with self.assertRaises(ValueError):
            self.plan(trace_mode="unknown")

    @unittest.skipUnless(build.platform.system() == "FreeBSD", "requires native meta-mode make")
    def test_native_trace_mode_preserves_structured_make_query_stdout(self):
        mode = next(arg.split("=", 1)[1] for arg in self.plan(trace_mode="filemon")["commands"]["buildworld"]
                    if arg.startswith(".MAKE.MODE="))
        query = self.parent / "query"
        query.mkdir()
        (query / "Makefile").write_text("OBJS=main.o\nloop:\n\t@echo 'OBJS= '${OBJS}\n")
        for name, value in (("quiet", mode), ("verbose-control", mode + " verbose")):
            objects = self.parent / name
            objects.mkdir()
            environment = {"PATH": build.PATH, "LC_ALL": "C", "HOME": str(self.parent),
                           "MAKEOBJDIR": str(objects)}
            result = subprocess.run(["/usr/bin/make", "-r", "-B", "-C", str(query),
                                     "loop", f".MAKE.MODE={value}"], env=environment,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if name == "quiet":
                self.assertEqual(result.stdout, "OBJS= main.o\n")
            else:
                self.assertIn("Building ", result.stdout)

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

    def test_large_archive_trace_is_preserved_without_a_whole_record_read(self):
        self.trace_fixture()
        archive = self.output / "obj/src.txz.meta"
        with archive.open("wb") as stream:
            stream.write(b"# Meta data file src.txz\n")
            for _ in range(17):
                stream.write(b"x" * 1024**2)
            stream.write(b"\n-- filemon acquired metadata --\nR 123 /source/file\n")
        observed = build.capture_trace_metadata(self.output, "sets")
        entry = next(e for e in observed["metadata"] if e["path"].endswith("src.txz.meta"))
        self.assertGreater(entry["size"], 16 * 1024**2)
        self.assertEqual(entry["sha256"], build.digest(archive))
        self.assertTrue(entry["filemon"])
        blob = self.output / "trace-blobs" / entry["sha256"]
        self.assertEqual(blob.stat().st_mode & 0o777, 0o444)
        build.verify_trace_metadata(self.output, [observed], ["sets"])

    def test_streamed_filemon_markers_can_cross_read_boundaries(self):
        path = self.trace_fixture()
        path.write_bytes(b"# Meta data file " + b"x" * 30 +
                         b"\n-- filemon acquired metadata --\nR 123 /source/file\n")
        with patch.object(build, "TRACE_CHUNK_SIZE", 32):
            observed = build.capture_trace_metadata(self.output, "buildworld")
        self.assertEqual(observed["compiled_targets_with_filemon"], 1)
        self.assertTrue(observed["metadata"][0]["filemon"])

    def test_streamed_trace_growth_is_refused_before_blob_publication(self):
        path = self.trace_fixture()
        real_fstat = build.os.fstat
        original = path.stat()
        changed = False

        def mutate_after_initial_stat(fd):
            nonlocal changed
            info = real_fstat(fd)
            if not changed and (info.st_dev, info.st_ino) == (original.st_dev, original.st_ino):
                changed = True
                with path.open("ab") as stream:
                    stream.write(b"R 123 /changed\n")
            return info

        with patch.object(build.os, "fstat", side_effect=mutate_after_initial_stat):
            with self.assertRaisesRegex(ValueError, "changed"):
                build.capture_trace_metadata(self.output, "buildworld")
        self.assertEqual(list((self.output / "trace-blobs").iterdir()), [])

    def test_trace_preflight_requires_existing_device_without_worker_mutation(self):
        with patch.object(build.platform, "system", return_value="FreeBSD"), \
                patch.object(build.platform, "machine", return_value="amd64"), \
                patch.object(build.platform, "release", return_value="16.0-CURRENT"), \
                patch.object(build.os, "geteuid", return_value=1001), \
                patch.object(build.os, "access", return_value=False), \
                patch.object(build.subprocess, "check_output") as process:
            with self.assertRaisesRegex(ValueError, "already configured"):
                build.native_preflight(self.parent, 1, 1, "filemon")
            process.assert_not_called()

    def test_optional_manifest_sets_cannot_escape_the_artifact_inventory(self):
        _, state = self.built_fixture()
        manifest = self.output / "sets/MANIFEST"
        optional = self.output / "sets/kernel-dbg.txz"
        optional.write_bytes(b"synthetic optional debug set")
        manifest.write_text(manifest.read_text() + f'kernel-dbg.txz\t{build.digest(optional)}\t1\tkernel_dbg\t"debug"\ton\n')
        with self.assertRaisesRegex(ValueError, "closure"):
            build.verify_manifest_closure(self.output, state["artifacts"])
        state["artifacts"].append({"path": "sets/kernel-dbg.txz", "size": optional.stat().st_size,
                                   "sha256": build.digest(optional)})
        build.verify_manifest_closure(self.output, state["artifacts"])
        optional.write_bytes(b"changed debug set")
        with self.assertRaisesRegex(ValueError, "changed"):
            build.verify_manifest_closure(self.output, state["artifacts"])

    def test_manifest_unsafe_duplicate_and_malformed_references_refuse(self):
        self.built_fixture()
        path = self.output / "sets/MANIFEST"
        original = path.read_text()
        for data in (original + original.splitlines()[0] + "\n",
                     original.replace("base.txz", "../base.txz"),
                     original.replace("\t1\t", "\tzero\t"), ""):
            with self.subTest(data=data[:30]):
                path.write_text(data)
                with self.assertRaises(ValueError):
                    build.manifest_references(path)

    def test_manifest_redirected_parent_refuses_before_reading_references(self):
        _, state = self.built_fixture()
        actual = self.output / "sets"
        moved = self.parent / "moved-sets"
        actual.rename(moved)
        actual.symlink_to(moved, target_is_directory=True)
        with patch.object(build, "manifest_references") as read:
            with self.assertRaisesRegex(ValueError, "redirected"):
                build.verify_manifest_closure(self.output, state["artifacts"])
            read.assert_not_called()

    @unittest.skipUnless(build.platform.system() == "FreeBSD", "requires native FreeBSD release make")
    def test_actual_release_tree_archives_preserve_symlinks_and_root_ownership(self):
        repository = Path(__file__).resolve().parents[2]
        (self.source / "share").symlink_to(repository / "share", target_is_directory=True)
        (self.source / "real.c").write_text("retained source\n")
        (self.source / "alias.c").symlink_to("real.c")
        outside = self.parent / "external"
        outside.mkdir()
        (outside / "must-not-follow").write_text("fixture outside source\n")
        (self.source / "external-link").symlink_to(outside, target_is_directory=True)
        self.output.mkdir()
        for include in (repository / "release").glob("Makefile.*"):
            (self.output / include.name).symlink_to(include)
        (self.parent / "sys/conf").mkdir(parents=True)
        (self.parent / "sys/conf/newvers.sh").write_text(
            "TYPE=FreeBSD\nREVISION=fixture\nBRANCH=CURRENT\n")
        environment = dict(os.environ, MAKEOBJDIR=str(self.output), LC_ALL="C")
        result = subprocess.run([
            "/usr/bin/make", "-m", str(repository / "share/mk"), "-C", str(self.output),
            "-f", str(repository / "release/Makefile"), "src.txz", "ports.txz", "TARGET=amd64",
            "TARGET_ARCH=amd64", "OSRELEASE=fixture", "NOPKGBASE=yes", "NOPORTS=yes",
            "NO_ROOT=yes", ".MAKE.MODE=normal", "MK_AUTO_OBJ=no", "XZ_THREADS=1",
            "__MAKE_CONF=/dev/null", "SRC_ENV_CONF=/dev/null", "SRCCONF=/dev/null",
            f"WORLDDIR={self.source}", f"PORTSDIR={self.source}",
        ], env=environment, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for tree in ("src", "ports"):
            with self.subTest(tree=tree):
                with tarfile.open(self.output / f"{tree}.txz", "r:xz") as archive:
                    members = {m.name.rstrip("/"): m for m in archive}
                self.assertTrue(members[f"usr/{tree}/alias.c"].issym())
                self.assertEqual(members[f"usr/{tree}/alias.c"].linkname, "real.c")
                self.assertTrue(members[f"usr/{tree}/external-link"].issym())
                self.assertNotIn(f"usr/{tree}/external-link/must-not-follow", members)
                self.assertIn(f"usr/{tree}/real.c", members)
                self.assertTrue(all(m.uid == 0 and m.gid == 0 for m in members.values()))


class SnapshotTests(unittest.TestCase):
    """Source without .git: identity pinned by the raw commit object, content checked against its tree."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.parent = Path(self.temporary.name).resolve()
        repo = self.parent / "repo"
        repo.mkdir()
        for name, content in {
            "tools/daemonarchy/build.py": "# controller\n",
            "tools/daemonarchy/src.conf": "WITHOUT_LOCAL_MODULES=yes\n",
            "sys/amd64/conf/GENERIC": "ident GENERIC\n",
            "sys/conf/newvers.sh": 'REVISION="16.0"\n',
            "bin/run.sh": "#!/bin/sh\n",
        }.items():
            path = repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        (repo / "bin/run.sh").chmod(0o755)
        (repo / "bin/link").symlink_to("run.sh")
        (repo / "empty").mkdir()
        self.git_in = lambda *a: subprocess.check_output(["git", "-C", str(repo), *a], text=True).strip()
        self.git_in("init", "-q")
        self.git_in("config", "user.email", "test@example.invalid")
        self.git_in("config", "user.name", "Test")
        self.git_in("add", ".")
        self.git_in("commit", "-qm", "fixture")
        self.revision = self.git_in("rev-parse", "HEAD")
        self.commit = self.parent / "commit.obj"
        self.commit.write_bytes(subprocess.check_output(["git", "-C", str(repo), "cat-file", "commit", "HEAD"]))
        self.source = self.parent / "snap"
        subprocess.check_call(["git", "-C", str(repo), "checkout-index", "-a", "--prefix", str(self.source) + "/"])
        self.output = self.parent / "output"
        self.upstream = "1" * 40
        self.args = build.argparse.Namespace(snapshot_commit=str(self.commit),
                                             snapshot_upstream_base=self.upstream, snapshot_exclude=None)
        patcher = patch.object(build, "SOURCE", self.source)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.addCleanup(lambda: setattr(build, "SNAPSHOT", None))

    def plan(self, **kwargs):
        return build.make_plan(kwargs.get("revision", self.revision), str(self.output), "baseline", 2, "off")

    def pin(self, **kwargs):
        for key, value in kwargs.items():
            setattr(self.args, key, value)
        build.SNAPSHOT = build.load_snapshot(self.args)

    def test_snapshot_plans_with_pinned_identity(self):
        self.pin()
        plan = self.plan()
        self.assertEqual(plan["source_revision"], self.revision)
        self.assertEqual(plan["source_tree"], self.git_in("rev-parse", "HEAD^{tree}"))
        self.assertEqual(plan["upstream_base"], self.upstream)
        self.assertEqual(plan["environment"]["SOURCE_DATE_EPOCH"], self.git_in("show", "-s", "--format=%ct"))

    def test_snapshot_packages_bind_complete_file_list_and_refuse_tampering(self):
        self.pin()
        plan = self.plan()
        self.assertFalse(self.output.exists())
        specification = plan["source_package_files"]
        expected = self.git_in("ls-files", "-z").encode().split(b"\0")
        expected = b"".join(name + b"\n" for name in sorted(filter(None, expected)))
        self.assertEqual(specification["sha256"], build.hashlib.sha256(expected).hexdigest())
        self.assertEqual(specification["count"], expected.count(b"\n"))
        self.assertIn("PKG_SOURCE_FILE_LIST=" + specification["path"], plan["commands"]["packages"])
        self.output.mkdir()
        build.materialize_snapshot_package_files(plan)
        manifest = Path(specification["path"])
        self.assertEqual(manifest.read_bytes(), expected)
        manifest.write_bytes(b"bin/run.sh\n")
        with self.assertRaisesRegex(ValueError, "file list changed"):
            build.materialize_snapshot_package_files(plan)

    def test_tree_computation_matches_git(self):
        self.assertEqual(build.snapshot_tree(self.source, []).hex(), self.git_in("rev-parse", "HEAD^{tree}"))

    def test_wrong_revision_refused(self):
        self.pin()
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.plan(revision="0" * 40)

    def test_modified_added_deleted_or_mode_changed_refused(self):
        self.pin()
        target = self.source / "sys/conf/newvers.sh"
        original = target.read_text()
        for mutate, restore in (
            (lambda: target.write_text(original + "#\n"), lambda: target.write_text(original)),
            (lambda: (self.source / "extra").write_text("x"), lambda: (self.source / "extra").unlink()),
            (lambda: target.unlink(), lambda: target.write_text(original)),
            (lambda: (self.source / "bin/run.sh").chmod(0o644), lambda: (self.source / "bin/run.sh").chmod(0o755)),
        ):
            mutate()
            with self.assertRaisesRegex(ValueError, "pinned commit tree"):
                self.plan()
            restore()
        self.plan()

    def test_exclude_allows_declared_extras_only(self):
        (self.source / "closure").mkdir()
        (self.source / "closure/lib").write_text("x")
        self.pin()
        with self.assertRaisesRegex(ValueError, "pinned commit tree"):
            self.plan()
        self.pin(snapshot_exclude=["closure"])
        self.assertEqual(self.plan()["source_snapshot"]["excludes"], ["closure"])
        self.assertNotIn(b"closure/lib\n", build.snapshot_package_files())
        (self.source / "other").write_text("x")
        with self.assertRaisesRegex(ValueError, "pinned commit tree"):
            self.plan()

    def test_unsafe_options_refused(self):
        for exclude in (["tools"], ["tools/daemonarchy/build.py"], ["/abs"], ["../x"], ["a/../b"], ["a/"]):
            with self.subTest(exclude=exclude), self.assertRaises(ValueError):
                self.pin(snapshot_exclude=exclude)
        self.args.snapshot_exclude = None
        self.pin(snapshot_exclude=None)
        with self.assertRaises(ValueError):
            self.pin(snapshot_upstream_base="main")
        self.args.snapshot_upstream_base = self.upstream
        bad = self.parent / "bad.obj"
        bad.write_bytes(b"not a commit")
        with self.assertRaises(ValueError):
            self.pin(snapshot_commit=str(bad))

    def test_no_identity_without_git_refused(self):
        build.SNAPSHOT = None
        with self.assertRaisesRegex(ValueError, "no .git"):
            self.plan()
        self.args.snapshot_commit = None
        with self.assertRaisesRegex(ValueError, "--snapshot-commit"):
            build.load_snapshot(self.args)

    def test_snapshot_options_refused_when_git_present(self):
        (self.source / ".git").mkdir()
        with self.assertRaisesRegex(ValueError, "without .git"):
            build.load_snapshot(self.args)
        self.args.snapshot_commit = self.args.snapshot_upstream_base = None
        self.assertIsNone(build.load_snapshot(self.args))


if __name__ == "__main__":
    unittest.main()
