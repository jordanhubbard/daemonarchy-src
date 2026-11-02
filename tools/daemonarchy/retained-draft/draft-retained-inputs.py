#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""DRAFT generator for the daemonarchy-src whole-tree retained-project manifest and profile.

Run with the Literate AI runtime's interpreter (it imports literate_ai) from the
project root, after capturing the reviewed bootstrap seed into the ignored
tools/daemonarchy/retained/bootstrap directory (see README.md "Selected-unit retained
entry point"):

    litai-venv/bin/python tools/daemonarchy/retained-draft/draft-retained-inputs.py \
        --manifest-out /abs/path/outside/tree/manifest.draft.json

It writes the two tracked draft authority files next to itself (profile and test
policy) and the full inventory manifest (about 120,000 members, NOT tracked) to
--manifest-out.  It reads, hashes and lists; it runs nothing from the tree and asserts
no build, test or acceptance result.  See PROVENANCE.md for every unknown.
"""
import argparse
import ast
import hashlib
import json
import os
import stat
import sys
from pathlib import Path

from literate_ai.contracts.execution_dispatch import ExecutionWorker, ExecutionWorkerKind
from literate_ai.contracts.retained_project import (
    RetainedProjectLimits,
    RetainedProjectManifest,
    RetainedProjectMember,
    RetainedProjectTreePolicy,
)
from literate_ai.contracts.retained_project_lifecycle import RetainedProjectProfile
from literate_ai.contracts.retained_project_tests import RetainedProjectTestPolicy
from literate_ai.projects import ProjectConfigurationStore

HERE = "tools/daemonarchy/retained-draft"
PROFILE = HERE + "/profile.draft.json"
POLICY = HERE + "/test-policy.draft.json"
REPORT_TOOL = HERE + "/unittest-report.py"
RELEASE = "literate.release.json"
BOOTSTRAP = "tools/daemonarchy/retained/bootstrap"
TARGET = "freebsd"
# _build: litai cache (git-ignored); retained-out: sealed outputs, not inputs.
EXCLUDED_TOP = {".git", ".literate", "literate.project.json", "retained-out", "_build"}
TEST_DIR = "tools/daemonarchy"
# The controller suite CI runs verbatim:
#   python3 -m unittest discover -s tools/daemonarchy -p 'test_*.py' -v
# (the clang-built tests under tools/daemonarchy/tests are not in this inventory).
TEST_FILES = tuple(sorted(str(p) for p in Path(TEST_DIR).glob("test_*.py")))
REVISION = "1924bee0c02ffede04c5002b5f7b46b44814558f"  # src rebind PR head


def role_of(path: str) -> str:
    if path == BOOTSTRAP or path.startswith(BOOTSTRAP + "/"):
        return "toolchain"
    if path == RELEASE:
        return "release-policy"
    if path == POLICY:
        return "test-policy"
    if path in (PROFILE, "Makefile") or path.startswith(("share/mk/", "tools/daemonarchy/build.py")):
        return "build-definition"
    if path in TEST_FILES or path == REPORT_TOOL:
        return "test-definition"
    if path.startswith("share/man/") or path.endswith(".md") and "/" not in path:
        return "docs-definition"
    return "source"


def sha256(path: Path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def inventory(root: Path, roots):
    members, dirs = [], []

    def visit(rel: str):
        full = root / rel
        st = os.lstat(full)
        if stat.S_ISLNK(st.st_mode):
            target = os.readlink(full)
            data = target.encode()
            members.append(RetainedProjectMember(rel, len(data), 0o120000,
                hashlib.sha256(data).hexdigest(), role_of(rel), symlink_target=target))
        elif stat.S_ISDIR(st.st_mode):
            dirs.append(rel)
            for name in os.listdir(full):
                visit(rel + "/" + name)
        elif stat.S_ISREG(st.st_mode):
            mode = 0o755 if st.st_mode & 0o111 else 0o644
            members.append(RetainedProjectMember(rel, st.st_size, mode,
                sha256(full), role_of(rel)))
        else:
            raise SystemExit("special file in retained roots: " + rel)

    for r in roots:
        visit(r)
    members.sort(key=lambda m: m.path)
    return tuple(members), tuple(sorted(dirs))


def test_cases():
    cases = []
    for name in TEST_FILES:
        tree = ast.parse(Path(name).read_text())
        module = Path(name).stem
        for node in tree.body:
            if isinstance(node, ast.ClassDef):
                for item in node.body:
                    if isinstance(item, ast.FunctionDef) and item.name.startswith("test"):
                        cases.append(f"{module}.{node.name}.{item.name}")
    return tuple(sorted(set(cases)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest-out", required=True)
    args = ap.parse_args()
    root = Path.cwd().resolve()
    roots = tuple(sorted(n for n in os.listdir(root) if n not in EXCLUDED_TOP))
    definition = ProjectConfigurationStore(root).read().definition

    cases = test_cases()
    policy = RetainedProjectTestPolicy.from_dict({
        "schema": "literate-ai/retained-project-test-policy@1",
        "project_policy_identity": definition.test_receipt_policy.identity.uri,
        "cases": [{"id": c, "action": "draft-controller-tests", "skip_reason": None}
                  for c in cases],
        "reports": [{"action": "draft-controller-tests",
                     "path": "tests/controller-report.json"}],
    })
    (root / POLICY).write_text(json.dumps(policy.to_dict(), indent=2) + "\n")

    limits = RetainedProjectLimits(1_000_000, 512 * 1024 * 1024, 4 * 1024**3)
    tree = RetainedProjectTreePolicy(case_sensitive=True)
    profile_stub = root / PROFILE
    profile_stub.write_text("{}\n")
    members, dirs = inventory(root, roots)
    provisional = RetainedProjectManifest(roots, members, limits, dirs, (), tree)
    by_path = {m.path: m for m in members}

    def tool(name, rel):
        m = by_path[BOOTSTRAP + "/tree/" + rel]
        return {"name": name, "path": m.path, "identity": "sha256:" + m.sha256}

    worker = ExecutionWorker("local", ExecutionWorkerKind.LOCAL, target_profile=TARGET)
    release_member = by_path[RELEASE]
    policy_member = by_path[POLICY]
    profile = {
        "schema": "literate-ai/retained-project-profile@1",
        "authority_mode": "retained-project",
        "target": TARGET,
        "worker_identity": worker.identity.uri,
        "toolchain_identity": provisional.role_identity({"sdk", "toolchain"}).uri,
        "dependency_identity": provisional.role_identity({"dependency"}).uri,
        "test_policy_identity": "sha256:" + policy_member.sha256,
        "release_policy_identity": "sha256:" + release_member.sha256,
        "tools": [tool("python", "usr/local/bin/python3.12"),
                  tool("echo", "bin/echo")],
        "actions": [
            # Grounded: tools/daemonarchy/README.md "Plan and build" (phases buildworld,
            # buildkernel, packages, release-obj, sets) with the DEV-001 baseline's four
            # jobs.  Whether the controller runs from a retained input copy without .git
            # is UNKNOWN; so is its --revision/clean-checkout check against this tree.
            {"id": "draft-controller-build", "stage": "build", "tool": "python",
             "argv": [BOOTSTRAP + "/tree/usr/local/bin/python3.12",
                      "tools/daemonarchy/build.py", "run", "--revision", REVISION,
                      "--output", "{output_root}/build", "--jobs", "4"],
             "requires": [],
             "inventory": ["buildworld", "buildkernel", "packages", "release-obj", "sets"],
             "outputs": ["build"], "timeout_seconds": 86400},
            # Grounded: the repository CI's controller test command; the report producer
            # records outcomes observed at run time.
            {"id": "draft-controller-tests", "stage": "test", "tool": "python",
             "argv": [BOOTSTRAP + "/tree/usr/local/bin/python3.12", REPORT_TOOL,
                      "--test-dir", TEST_DIR,
                      "--output", "{output_root}/tests/controller-report.json"],
             "requires": ["draft-controller-build"], "inventory": list(cases),
             "outputs": ["tests"], "timeout_seconds": 3600},
            # UNKNOWN: no whole-tree docs build or check is defined for this fork.
            {"id": "draft-unknown-docs", "stage": "docs", "tool": "echo",
             "argv": [BOOTSTRAP + "/tree/bin/echo", "UNKNOWN-placeholder-not-a-result"],
             "requires": ["draft-controller-build"],
             "inventory": ["UNKNOWN-source-docs-action"],
             "outputs": ["documentation"], "timeout_seconds": 3600},
            # UNKNOWN: verification/retained-project-acceptance.json covers eleven
            # selected-utility cases only; no whole-build acceptance policy exists.
            {"id": "draft-unknown-accept", "stage": "accept", "tool": "echo",
             "argv": [BOOTSTRAP + "/tree/bin/echo", "UNKNOWN-placeholder-not-a-result"],
             "requires": ["draft-controller-build", "draft-controller-tests",
                          "draft-unknown-docs"],
             "inventory": ["UNKNOWN-independent-acceptance"],
             "outputs": ["acceptance"], "timeout_seconds": 3600},
        ],
        "package_inventory": ["build"],
        "runtime_library_paths": [BOOTSTRAP + "/tree/lib", BOOTSTRAP + "/tree/usr/lib",
                                  BOOTSTRAP + "/tree/usr/local/lib"],
    }
    RetainedProjectProfile.from_dict(profile)
    profile_stub.write_text(json.dumps(profile, indent=2) + "\n")

    members, dirs = inventory(root, roots)
    manifest = RetainedProjectManifest(roots, members, limits, dirs, (), tree)
    out = Path(args.manifest_out)
    out.write_text(json.dumps(manifest.to_dict(), separators=(",", ":")) + "\n")
    print(json.dumps({
        "roots": len(roots), "members": len(members), "directories": len(dirs),
        "symlinks": sum(1 for m in members if m.symlink_target is not None),
        "toolchain_members": sum(1 for m in members if m.role == "toolchain"),
        "total_bytes": sum(m.size for m in members),
        "test_cases": len(cases),
        "manifest_identity": manifest.identity.uri,
        "profile_identity": RetainedProjectProfile.from_dict(profile).identity.uri,
        "worker_identity": worker.identity.uri,
        "toolchain_identity": profile["toolchain_identity"],
        "dependency_identity": profile["dependency_identity"],
        "test_policy_identity": profile["test_policy_identity"],
        "release_policy_identity": profile["release_policy_identity"],
    }, indent=2))


if __name__ == "__main__":
    sys.exit(main())
