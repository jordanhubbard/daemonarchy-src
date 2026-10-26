"""Compile host fixtures around verbatim production C functions.

Each selection names a source file and a regular expression that must match
exactly once; the matched text replaces the fixture's single
`/* PRODUCTION_FUNCTIONS */` marker.  The build uses address and undefined
behavior sanitizers, and every named case runs as a separate process.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess

SOURCE_ROOT = Path(__file__).resolve().parents[3]


def function(name: str, prefix: str) -> str:
    """Pattern for a top-level function whose return type line is `prefix`."""
    return r"(?ms)^" + prefix + r"\n" + name + r"\(.*?^\}\n"


def run(fixture: str, selections, cases, scope: str, output_directory: Path,
        cflags=(), source_root: Path = SOURCE_ROOT) -> int:
    fragments, sources = [], {}
    for relative, label, pattern in selections:
        path = source_root / relative
        sources.setdefault(relative, hashlib.sha256(path.read_bytes()).hexdigest())
        matches = list(re.finditer(pattern, path.read_text()))
        if len(matches) != 1:
            raise SystemExit("production boundary is missing or ambiguous: " + label)
        fragments.append(matches[0].group())
    root = output_directory.resolve()
    root.mkdir(mode=0o700, parents=True)
    text = (Path(__file__).with_name(fixture)).read_text()
    if text.count("/* PRODUCTION_FUNCTIONS */") != 1:
        raise SystemExit("fixture must contain one production marker")
    (root / "fixture.c").write_text(
        text.replace("/* PRODUCTION_FUNCTIONS */", "\n".join(fragments)))
    argv = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall",
            "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", *cflags,
            "-I" + str(source_root), str(root / "fixture.c"), "-o",
            str(root / "fixture")]
    report = {"source_sha256": sources,
              "function_sha256": [hashlib.sha256(x.encode()).hexdigest()
                                  for x in fragments],
              "scope": scope, "build_exit_code": None, "cases": []}
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with (root / "build.stdout").open("xb") as out, \
            (root / "build.stderr").open("xb") as err:
        build = subprocess.run(argv, stdout=out, stderr=err, timeout=120, check=False)
    report["build_exit_code"] = build.returncode
    if build.returncode == 0:
        for name in cases:
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
            with (root / (name + ".stdout")).open("xb") as out, \
                    (root / (name + ".stderr")).open("xb") as err:
                result = subprocess.run([str(root / "fixture"), name], stdout=out,
                                        stderr=err, env=env, timeout=60, check=False)
            report["cases"].append({"name": name, "exit_code": result.returncode})
    else:
        print((root / "build.stderr").read_text()[-4000:])
    (root / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0 if build.returncode == 0 and all(
        c["exit_code"] == 0 for c in report["cases"]) else 1
