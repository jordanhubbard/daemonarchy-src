#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""DRAFT: run `build.py verify` on a sealed controller build and record the observation.

build.py verify re-inventories the build output and checks every successful phase and
artifact byte against result.json, but it prints to stdout and writes nothing, so a
retained action with a declared output needs this wrapper.  It is the SAME controller
that produced the build: unsigned local records, not independent evidence, and it says
nothing about booting, installing or hardware.  Exit status is build.py's.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-py", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--report", required=True)
    args = ap.parse_args()
    r = subprocess.run([sys.executable, "-B", args.build_py, "verify", "--output", args.build_dir],
                       capture_output=True, timeout=3600)
    out = Path(args.report)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({
        "schema": "daemonarchy/build-verify-observation@1",
        "exit_code": r.returncode,
        "stdout_sha256": hashlib.sha256(r.stdout).hexdigest(),
        "stdout_bytes": len(r.stdout),
        "stderr": r.stderr.decode("utf-8", "replace")[-4096:],
    }, indent=1) + "\n")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
