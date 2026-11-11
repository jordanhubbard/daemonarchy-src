#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""DRAFT: refuse a retained run whose ambient host inputs differ from the reviewed inventory.

The captured tree cannot hold files that executables name by absolute path (the dynamic
linker, shebang interpreters, poudriere's jails and package sets, public signed artifacts).
closure-inventory.json records their identity; this action re-observes each one and exits
nonzero on any difference or absence.  It reads and hashes only, writes one report under
--output, and never reads or names a signing key.
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def tree_digest(source, exclude=()):
    h = hashlib.sha256()
    files = nbytes = 0
    for base, dirs, names in os.walk(source):
        dirs[:] = sorted(d for d in dirs if d not in exclude)
        for name in sorted(names):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, source)
            if os.path.islink(full) and not os.path.isfile(full):
                raise SystemExit("symlink to a non-file inside a captured tree: " + full)
            st = os.stat(full)
            h.update(b"F\0" + rel.encode() + b"\0" + str(st.st_size).encode() + b"\0" +
                     ("%o" % (st.st_mode & 0o111 and 0o755 or 0o644)).encode() + b"\0" +
                     sha256_file(full).encode() + b"\n")
            files += 1
            nbytes += st.st_size
    return {"files": files, "bytes": nbytes, "digest": h.hexdigest()}


def listing_digest(path):
    names = sorted(os.listdir(path))
    h = hashlib.sha256()
    for n in names:
        h.update(("%s\0%d\n" % (n, os.lstat(os.path.join(path, n)).st_size)).encode())
    return {"entries": len(names), "digest": h.hexdigest()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inventory", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    inv = json.loads(Path(args.inventory).read_text())
    results = []
    for a in inv["ambient"]:
        path, kind = a["path"], a["kind"]
        outcome = {"name": a["name"], "path": path, "kind": kind}
        try:
            if not os.path.lexists(path):
                outcome.update(ok=False, reason="absent")
            elif kind == "file":
                real = os.path.realpath(path)
                outcome.update(ok=sha256_file(real) == a["sha256"], expected=a["sha256"])
            elif kind == "tree":
                got = tree_digest(path, set(a.get("exclude", [])))
                outcome.update(ok=got["digest"] == a["digest"], expected=a["digest"])
            elif kind == "listing":
                got = listing_digest(path)
                outcome.update(ok=got["digest"] == a["digest"], expected=a["digest"])
            else:
                outcome.update(ok=False, reason="unknown kind")
        except OSError as exc:
            outcome.update(ok=False, reason=str(exc))
        results.append(outcome)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"schema": "daemonarchy/ambient-preflight@1",
                               "results": results}, indent=1) + "\n")
    bad = [r["name"] for r in results if not r["ok"]]
    if bad:
        print("ambient inputs differ from the reviewed inventory: " + ", ".join(bad), file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
