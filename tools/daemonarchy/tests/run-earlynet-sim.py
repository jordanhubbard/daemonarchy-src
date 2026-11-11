#!/usr/bin/env python3
"""Compile the unchanged early transport sources against simulated NICs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import subprocess

SOURCES = ["sys/dev/daemonarchy/earlynet/earlynet_core.c",
           "sys/dev/daemonarchy/earlynet/earlynet_em.c",
           "sys/dev/daemonarchy/earlynet/earlynet_vtnet.c"]
CASES = ["em-roundtrip", "vtnet-roundtrip", "lost-ack-bounded", "degraded-line-flush", "no-controller",
         "em-handoff", "vtnet-handoff", "failed-quiesce", "malformed", "vtnet-bad-queue", "em-unprogrammed"]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-directory", type=Path, required=True)
args = parser.parse_args()
source_root = Path(__file__).resolve().parents[3]
root = args.output_directory.resolve()
root.mkdir(mode=0o700, parents=True)
argv = [*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11", "-Wall", "-Wextra",
        "-Werror", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer", "-I" + str(source_root),
        str(source_root / "tools/daemonarchy/tests/earlynet-sim.c"),
        *(str(source_root / s) for s in SOURCES), "-o", str(root / "earlynet-sim")]
report = {"source_sha256": {s: hashlib.sha256((source_root / s).read_bytes()).hexdigest()
                            for s in SOURCES},
          "scope": "Unchanged earlynet core, em and vtnet backends against simulated "
                   "devices and a scripted controller; not a kernel build, real NIC "
                   "or native boot",
          "build_exit_code": None, "cases": []}
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with (root / "build.stdout").open("xb") as out, (root / "build.stderr").open("xb") as err:
    build = subprocess.run(argv, stdout=out, stderr=err, timeout=120, check=False)
report["build_exit_code"] = build.returncode
if build.returncode == 0:
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    for name in CASES:
        with (root / f"{name}.stdout").open("xb") as out, \
                (root / f"{name}.stderr").open("xb") as err:
            result = subprocess.run([str(root / "earlynet-sim"), name], stdout=out,
                                    stderr=err, env=env, timeout=120, check=False)
        report["cases"].append({"name": name, "exit_code": result.returncode})
else:
    print((root / "build.stderr").read_text()[-4000:])
(root / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
raise SystemExit(0 if build.returncode == 0 and all(
    c["exit_code"] == 0 for c in report["cases"]) else 1)
