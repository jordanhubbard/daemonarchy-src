#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Pinned, unprivileged FreeBSD base builds. No installation or publication."""

import argparse
import contextlib
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import time


SOURCE = Path(__file__).resolve().parents[2]
PHASES = ("buildworld", "buildkernel", "packages", "release-obj", "sets")
PATH = "/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/sbin:/usr/local/bin"
SCHEMA = "daemonarchy/source-build-plan@1"


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def git(*args):
    return subprocess.check_output(
        ["git", "-C", str(SOURCE), *args], text=True, timeout=120
    ).strip()


def clean_revision(revision):
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("--revision must be a full 40-character commit ID")
    if git("rev-parse", "HEAD") != revision:
        raise ValueError("checkout does not match --revision")
    if git("status", "--porcelain=v1", "--untracked-files=normal"):
        raise ValueError("source checkout must be clean, including untracked files")
    if any(line.startswith("160000 ") for line in git("ls-files", "--stage").splitlines()):
        raise ValueError("submodules are not supported")


def safe_path(value):
    path = Path(value)
    if not path.is_absolute():
        raise ValueError("build paths must be absolute")
    path = path.resolve()
    # Upstream make recipes do not consistently quote whitespace/metacharacters.
    if not re.fullmatch(r"/[A-Za-z0-9_./:-]+", str(path)):
        raise ValueError("build paths must contain only make-safe characters")
    return path


def make_plan(revision, output, profile, jobs):
    clean_revision(revision)
    root = safe_path(output)
    safe_path(str(SOURCE))
    if root == Path("/") or root == SOURCE or root in SOURCE.parents or SOURCE in root.parents:
        raise ValueError("output must be outside the source tree and its ancestors")
    if not 1 <= jobs <= 256:
        raise ValueError("jobs must be between 1 and 256")
    kernel = {"baseline": "GENERIC", "diagnostic": "DAEMONARCHY_DIAGNOSTIC"}[profile]
    config = SOURCE / "tools/daemonarchy/src.conf"
    kernel_config = SOURCE / "sys/amd64/conf" / kernel
    version_text = (SOURCE / "sys/conf/newvers.sh").read_text()
    match = re.search(r'^REVISION="([0-9.]+)"', version_text, re.M)
    if not match:
        raise ValueError("cannot identify source release version")
    stamp = datetime.datetime.fromtimestamp(
        int(git("show", "-s", "--format=%ct", revision)), datetime.timezone.utc
    ).strftime("%Y%m%d%H%M%S")
    version = f"{match[1]}.snap{stamp}.{revision[:12]}.{profile}"
    common = [
        "TARGET=amd64", "TARGET_ARCH=amd64", f"KERNCONF={kernel}",
        "__MAKE_CONF=/dev/null", "SRC_ENV_CONF=/dev/null", f"SRCCONF={config}",
        "NO_ROOT=yes", "DB_FROM_SRC=yes", "WITHOUT_LOCAL_MODULES=yes",
        f"REPODIR={root / 'repo'}", f"PKG_VERSION={version}",
        f"WSTAGEDIR={root / 'worldstage'}", f"KSTAGEDIR={root / 'kernelstage'}",
        f"SSTAGEDIR={root / 'sourcestage'}", "PKG_REPO_SIGNING_KEY=",
        "PKG_CMD=/usr/local/sbin/pkg",
    ]
    make = ["/usr/bin/make", "-C", str(SOURCE), *common]
    release = ["/usr/bin/make", "-C", str(SOURCE / "release"), *common,
               f"WORLDDIR={SOURCE}", "NOPORTS=yes", "NOPKGBASE=yes"]
    commands = {
        "buildworld": [*make, f"-j{jobs}", "buildworld"],
        "buildkernel": [*make, f"-j{jobs}", "buildkernel"],
        "packages": [*make, f"-j{jobs}", "packages"],
        "release-obj": [*release, "obj"],
        "sets": [*release, "packagesystem"],
    }
    plan = {
        "schema": SCHEMA, "source_revision": revision, "source_tree": git("rev-parse", "HEAD^{tree}"),
        "source_directory": str(SOURCE), "source_repository": "https://github.com/jordanhubbard/daemonarchy-src",
        "upstream_repository": "https://github.com/freebsd/freebsd-src",
        "upstream_base": git("merge-base", revision, "refs/remotes/upstream/main"),
        "profile": profile, "kernel": kernel, "package_version": version,
        "target": "amd64", "target_arch": "amd64", "jobs": jobs, "output": str(root),
        "configurations": {str(p.relative_to(SOURCE)): digest(p) for p in (config, kernel_config)},
        "environment": {"PATH": PATH, "MAKEOBJDIRPREFIX": str(root / "obj"),
                        "HOME": str(root / "home"), "TMPDIR": str(root / "tmp"),
                        "LC_ALL": "C", "TZ": "UTC", "SOURCE_DATE_EPOCH": git("show", "-s", "--format=%ct", revision)},
        "commands": commands, "release_directory_query": [*release, "-V", ".OBJDIR"],
        "publication": "unsigned-development-only", "acceptance": "not-qualified",
    }
    plan["identity"] = hashlib.sha256(canonical(plan)).hexdigest()
    return plan


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def native_preflight(root, jobs, minimum_free_gib):
    if platform.system() != "FreeBSD" or platform.machine() not in ("amd64", "x86_64"):
        raise ValueError("execution requires a native amd64 FreeBSD worker; plan works on other hosts")
    if os.geteuid() == 0:
        raise ValueError("use an unprivileged build account")
    for tool in ("/usr/bin/make", "/usr/bin/cc", "/usr/local/sbin/pkg"):
        if not os.access(tool, os.X_OK):
            raise ValueError(f"required installed tool missing: {tool}")
    if shutil.disk_usage(root).free < minimum_free_gib * 1024**3:
        raise ValueError(f"at least {minimum_free_gib} GiB free required on output volume")
    if shutil.disk_usage("/tmp").free < 2 * 1024**3:
        raise ValueError("less than 2 GiB free on /tmp")
    if jobs > (os.cpu_count() or 1):
        raise ValueError("requested jobs exceeds worker CPU count")
    memory = [int(subprocess.check_output(["/sbin/sysctl", "-n", key], text=True, timeout=10))
              for key in ("vm.stats.vm.v_free_count", "vm.stats.vm.v_inactive_count", "hw.pagesize")]
    if (memory[0] + memory[1]) * memory[2] < max(8, jobs * 2) * 1024**3:
        raise ValueError("insufficient available memory: reserve at least 2 GiB per job and 8 GiB total")


def run_command(argv, env, log, timeout):
    with log.open("xb") as stream:
        child = subprocess.Popen(argv, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            return child.wait(timeout=timeout)
        except BaseException:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(child.pid, signal.SIGTERM)
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                with contextlib.suppress(ProcessLookupError):
                    os.killpg(child.pid, signal.SIGKILL)
                child.wait()
            raise


def artifacts(plan):
    root = Path(plan["output"])
    packages = sorted(p for p in (root / "repo").rglob("*.pkg") if not p.is_symlink() and p.is_file())
    required = (r"FreeBSD-kernel-", r"FreeBSD-runtime-", r"FreeBSD-src-[0-9]", r"FreeBSD-src-sys-")
    for pattern in required:
        if not any(re.match(pattern, p.name) for p in packages):
            raise ValueError(f"missing expected pkgbase package: {pattern}")
    sets = Path(subprocess.check_output(plan["release_directory_query"], env=plan["environment"],
                                        text=True, timeout=120).strip()).resolve()
    if root not in sets.parents:
        raise ValueError("release object directory escaped build output")
    files = packages + [sets / n for n in ("base.txz", "kernel.txz", "lib32.txz", "src.txz", "MANIFEST")]
    # Retain pkg repository catalogues as well as package bytes.
    files += [p for p in (root / "repo").rglob("*")
              if p.is_file() and not p.is_symlink() and p not in packages]
    entries = []
    for path in sorted(set(files)):
        if not path.is_file() or path.is_symlink() or path.stat().st_size == 0:
            raise ValueError(f"missing, empty or symlink artifact: {path}")
        if root not in path.resolve().parents:
            raise ValueError("artifact escaped output directory")
        entries.append({"path": str(path.relative_to(root)), "size": path.stat().st_size, "sha256": digest(path)})
    return entries


def require_inventory(entries):
    if not entries or len({e["path"] for e in entries}) != len(entries):
        raise ValueError("missing or duplicate artifact inventory")
    for item in entries:
        relative = Path(item["path"])
        if relative.is_absolute() or ".." in relative.parts or item["size"] <= 0:
            raise ValueError("invalid artifact inventory entry")
    names = [Path(e["path"]).name for e in entries]
    for name in ("base.txz", "kernel.txz", "lib32.txz", "src.txz", "MANIFEST"):
        if name not in names:
            raise ValueError(f"missing installer set: {name}")
    for pattern in (r"FreeBSD-kernel-", r"FreeBSD-runtime-", r"FreeBSD-src-[0-9]", r"FreeBSD-src-sys-"):
        if not any(re.match(pattern, n) and n.endswith(".pkg") for n in names):
            raise ValueError(f"missing pkgbase inventory: {pattern}")


def verify(root):
    root = safe_path(str(root))
    plan = json.loads((root / "plan.json").read_text())
    identity = plan.pop("identity")
    if plan.get("schema") != SCHEMA or hashlib.sha256(canonical(plan)).hexdigest() != identity:
        raise ValueError("plan identity mismatch")
    if plan["output"] != str(root):
        raise ValueError("build output location mismatch")
    state = json.loads((root / "result.json").read_text())
    if state.get("plan_identity") != identity or state.get("status") != "built":
        raise ValueError("build incomplete, failed or bound to a different plan")
    if [p["name"] for p in state["phases"] if p["exit"] == 0] != list(PHASES):
        raise ValueError("required successful phases missing")
    entries = state.get("artifacts", [])
    require_inventory(entries)
    for item in entries + [{"path": p["log"], "sha256": p["log_sha256"]} for p in state["phases"]]:
        path = root / item["path"]
        if path.is_symlink() or root not in path.resolve().parents or digest(path) != item["sha256"]:
            raise ValueError(f"retained artifact/log changed: {item['path']}")
        if "size" in item and path.stat().st_size != item["size"]:
            raise ValueError(f"artifact size changed: {item['path']}")
    return {"plan_identity": identity, "status": "bytes-verified", "artifacts": len(entries),
            "acceptance": "not-qualified", "publication": "unsigned-development-only"}


def interrupted(signum, frame):
    raise InterruptedError(f"interrupted by signal {signum}")


def run(plan, args):
    root = Path(plan["output"])
    if args.resume:
        if json.loads((root / "plan.json").read_text()) != plan:
            raise ValueError("resume plan differs from retained inputs")
    else:
        if not root.parent.is_dir():
            raise ValueError("output parent must already exist")
        native_preflight(root.parent, args.jobs, args.minimum_free_gib)
        root.mkdir()  # Exclusive; never overwrite or clean an existing build.
        write_json(root / "plan.json", plan)
    with (root / "build.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        native_preflight(root, args.jobs, args.minimum_free_gib)
        for name in ("obj", "repo", "home", "tmp", "logs"):
            (root / name).mkdir(exist_ok=True)
        state_path = root / "result.json"
        state = json.loads(state_path.read_text()) if args.resume else {
            "schema": "daemonarchy/source-build-result@1", "plan_identity": plan["identity"],
            "status": "incomplete", "phases": [], "artifacts": [],
            "worker": {"os": platform.platform(), "machine": platform.machine(),
                       "cpus": os.cpu_count(), "compiler": subprocess.check_output(
                           ["/usr/bin/cc", "--version"], text=True, timeout=30),
                       "make_version": subprocess.check_output(
                           ["/usr/bin/make", "-V", ".MAKE.VERSION"], text=True, timeout=30).strip(),
                       "pkg_version": subprocess.check_output(
                           ["/usr/local/sbin/pkg", "--version"], text=True, timeout=30).strip()},
            "acceptance": "not-qualified", "publication": "unsigned-development-only",
        }
        if state.get("plan_identity") != plan["identity"]:
            raise ValueError("result/plan mismatch")
        successful = [p["name"] for p in state["phases"] if p["exit"] == 0]
        if successful != list(PHASES[:len(successful)]):
            raise ValueError("completed phases are not an ordered prefix")
        for phase in state["phases"]:
            if digest(root / phase["log"]) != phase["log_sha256"]:
                raise ValueError("retained phase log changed")
        if state["status"] == "built":
            return verify(root)
        for phase in PHASES:
            clean_revision(plan["source_revision"])
            if phase not in successful:
                native_preflight(root, args.jobs, args.minimum_free_gib)
                attempt = 1 + sum(p["name"] == phase for p in state["phases"])
                log = root / "logs" / f"{phase}-{attempt}.log"
                state["status"] = "running"
                state.pop("error", None)
                state["active_phase"] = phase
                write_json(state_path, state)
                print(json.dumps({"phase": phase, "log": str(log), "plan_identity": plan["identity"]}), flush=True)
                started = time.time()
                try:
                    status = run_command(plan["commands"][phase], plan["environment"], log, args.phase_timeout)
                    clean_revision(plan["source_revision"])
                except BaseException as error:
                    status = -1
                    state["error"] = f"{type(error).__name__}: {error}"
                state["phases"].append({"name": phase, "exit": status, "log": str(log.relative_to(root)),
                                        "log_sha256": digest(log), "elapsed_seconds": round(time.time() - started, 3)})
                state["status"] = "incomplete" if status == 0 else "failed"
                state.pop("active_phase", None)
                write_json(state_path, state)
                if status != 0:
                    raise ValueError(f"{phase} failed; inspect {log}")
            if phase == args.through:
                break
        if args.through == "sets":
            state["artifacts"] = artifacts(plan)
            require_inventory(state["artifacts"])
            clean_revision(plan["source_revision"])
            state["status"] = "built"
        write_json(state_path, state)
        return {"status": state["status"], "output": str(root), "acceptance": "not-qualified"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("plan", "run", "status", "verify"))
    parser.add_argument("--output", required=True)
    parser.add_argument("--revision")
    parser.add_argument("--profile", choices=("baseline", "diagnostic"), default="baseline")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--through", choices=PHASES, default="sets")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--minimum-free-gib", type=int, default=80)
    parser.add_argument("--phase-timeout", type=int, default=21600)
    args = parser.parse_args()
    if args.minimum_free_gib < 1 or args.phase_timeout < 1:
        parser.error("resource and timeout limits must be positive")
    if args.operation in ("plan", "run") and not args.revision:
        parser.error("--revision is required for plan/run")
    try:
        if args.operation == "verify":
            result = verify(Path(args.output))
        elif args.operation == "status":
            result = json.loads((safe_path(args.output) / "result.json").read_text())
        else:
            plan = make_plan(args.revision, args.output, args.profile, args.jobs)
            result = plan if args.operation == "plan" else run(plan, args)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"source-build: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    raise SystemExit(main())
