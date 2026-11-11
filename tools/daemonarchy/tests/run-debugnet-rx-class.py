#!/usr/bin/env python3
"""Compile receive-buffer class checks with verbatim kernel functions."""

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
source_root = args.source_root.resolve()


def function(name: str, prefix: str) -> str:
    return r"(?ms)^" + prefix + r"\n" + name + r"\(.*?^\}\n"


# (source, label, pattern); every pattern must match exactly once.
selections = [
    (
        "sys/sys/mbuf.h",
        "m_getzone",
        function("m_getzone", r"static __inline uma_zone_t"),
    ),
    (
        "sys/kern/kern_mbuf.c",
        "debugnet reserve block",
        r"(?ms)^#ifdef DEBUGNET\n.*?^#endif /\* DEBUGNET \*/\n",
    ),
    (
        "sys/net/debugnet.c",
        "reserve sizing through debugnet_any_ifnet_update",
        (
            r"(?ms)^/\*\n \* Pre-allocated dump-time mbuf tracking\..*?"
            r"^debugnet_any_ifnet_update\(.*?^\}\n"
        ),
    ),
    (
        "sys/net/iflib.c",
        "iflib_debugnet_init",
        function("iflib_debugnet_init", "static void"),
    ),
    (
        "sys/net/iflib.c",
        "iflib_debugnet_event",
        function("iflib_debugnet_event", "static void"),
    ),
    (
        "sys/net/iflib.c",
        "iflib_fl_bufs_free",
        function("iflib_fl_bufs_free", "static void"),
    ),
]
fragments = []
sources = {}
for relative, label, pattern in selections:
    path = source_root / relative
    if relative not in sources:
        sources[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    matches = list(re.finditer(pattern, path.read_text()))
    if len(matches) != 1:
        raise SystemExit("production boundary is missing or ambiguous: " + label)
    fragments.append(matches[0].group())
root = args.output_directory.resolve()
root.mkdir(mode=0o700)
fixture = Path(__file__).with_name("debugnet-rx-class.c").read_text()
assert fixture.count("/* PRODUCTION_FUNCTIONS */") == 1
fixture = fixture.replace("/* PRODUCTION_FUNCTIONS */", "\n".join(fragments))
(root / "fixture.c").write_text(fixture)
argv = [
    *shlex.split(os.environ.get("CC", "cc")),
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-g",
    "-fsanitize=address,undefined",
    "-fno-omit-frame-pointer",
    str(root / "fixture.c"),
    "-o",
    str(root / "fixture"),
]
report = {
    "source_sha256": sources,
    "function_sha256": [hashlib.sha256(x.encode()).hexdigest() for x in fragments],
    "scope": "Verbatim debugnet reserve, activation, iflib event and free-list cleanup with fake UMA, DMA and refill; not a linked kernel, live driver or jumbo-NIC qualification",
    "build_exit_code": None,
    "cases": [],
}
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with (
    (root / "build.stdout").open("xb") as out,
    (root / "build.stderr").open("xb") as err,
):
    build = subprocess.run(argv, stdout=out, stderr=err, timeout=60, check=False)
report["build_exit_code"] = build.returncode
if build.returncode == 0:
    for name in ["matching", "jumbo-reserve", "mixed", "repeated"]:
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        with (
            (root / (name + ".stdout")).open("xb") as out,
            (root / (name + ".stderr")).open("xb") as err,
        ):
            result = subprocess.run(
                [str(root / "fixture"), name],
                stdout=out,
                stderr=err,
                env=env,
                timeout=30,
                check=False,
            )
        report["cases"].append({"name": name, "exit_code": result.returncode})
(root / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report))
raise SystemExit(
    0
    if build.returncode == 0 and all(x["exit_code"] == 0 for x in report["cases"])
    else 1
)
