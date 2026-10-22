#!/usr/bin/env python3
"""Compile allocator fixtures with verbatim kernel functions, not a model."""

from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--source-root", type=Path, default=Path(__file__).resolve().parents[3]
)
parser.add_argument("--output-directory", type=Path, required=True)
args = parser.parse_args()
source = args.source_root.resolve() / "sys/kern/kern_mbuf.c"
content = source.read_text()
fragments = []
for function in ["debugnet_mbuf_start", "debugnet_mbuf_finish"]:
    matches = list(
        re.finditer(r"(?ms)^(?:void|int)\n" + function + r"\(void\)\n\{.*?^\}", content)
    )
    if len(matches) != 1:
        raise SystemExit(
            "production function boundary is missing or ambiguous: " + function
        )
    fragments.append(matches[0].group())
root = args.output_directory.resolve()
root.mkdir(mode=0o700)
fixture = Path(__file__).with_name("debugnet-mbuf-pools.c").read_text()
assert fixture.count("/* PRODUCTION_FUNCTIONS */") == 1
fixture = fixture.replace("/* PRODUCTION_FUNCTIONS */", "\n\n".join(fragments))
(root / "fixture.c").write_text(fixture)
returns_error = fragments[0].startswith("int\n")
argv = [
    *shlex.split(os.environ.get("CC", "cc")),
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-g",
    "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer",
    "-DSTART_RETURNS_ERROR=" + str(int(returns_error)),
    str(root / "fixture.c"),
    "-o",
    str(root / "fixture"),
]
report = {
    "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "function_sha256": [hashlib.sha256(x.encode()).hexdigest() for x in fragments],
    "scope": "Verbatim activation/restoration functions with fake zone objects; not a linked kernel or driver qualification",
    "build_exit_code": None,
    "cases": [],
}
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with (
    (root / "build.stdout").open("xb") as out,
    (root / "build.stderr").open("xb") as err,
):
    build = subprocess.run(argv, stdout=out, stderr=err, timeout=60)
report["build_exit_code"] = build.returncode
if build.returncode == 0:
    for name, switches in [("ready", ["ready"]), ("unready-and-retry", [])]:
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        with (
            (root / (name + ".stdout")).open("xb") as out,
            (root / (name + ".stderr")).open("xb") as err,
        ):
            result = subprocess.run(
                [str(root / "fixture"), *switches],
                stdout=out,
                stderr=err,
                env=env,
                timeout=30,
            )
        report["cases"].append({"name": name, "exit_code": result.returncode})
(root / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
raise SystemExit(
    0
    if build.returncode == 0 and all(x["exit_code"] == 0 for x in report["cases"])
    else 1
)
