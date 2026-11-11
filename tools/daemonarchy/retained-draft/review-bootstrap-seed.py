#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""DRAFT: read-only mechanical review of a captured bootstrap seed.

Checks, without writing anything under the seed or the host:
  inventory   sha256 of inventory.json equals the digest recorded in capture.json
  tree        every inventory entry exists in <seed>/tree with the recorded kind, size,
              sha256, normalized mode (0644/0755) and symlink target, and nothing else
              exists in the tree
  host        each captured file still has the inventory's sha256 on the live host
              (drift since capture; informational, a changed host is not a seed defect)
  secrets     no captured path looks like private key material
  closure     every captured ELF's DT_NEEDED libraries (from `ldd` of the host file) are
              themselves in the seed, and its interpreter is listed (absolute host path)
It prints a JSON report.  It does not make the inventory "independently reviewed": the
capture requires a human reviewer's digest, and this script cannot supply that.

    review-bootstrap-seed.py --seed tools/daemonarchy/retained/bootstrap [--output report.json]
"""
import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from pathlib import Path


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seed", required=True)
    ap.add_argument("--output")
    args = ap.parse_args()
    seed = Path(args.seed)
    inv_bytes = (seed / "inventory.json").read_bytes()
    capture = json.loads((seed / "capture.json").read_text())
    inv = json.loads(inv_bytes)
    entries = inv["entries"]
    tree = seed / "tree"
    report = {"inventory_sha256": hashlib.sha256(inv_bytes).hexdigest(),
              "capture_records": capture["inventory_sha256"], "findings": []}
    bad = report["findings"]
    if report["inventory_sha256"] != capture["inventory_sha256"]:
        bad.append("inventory digest differs from capture.json")
    counts = {"entries": len(entries), "files": 0, "symlinks": 0, "directories": 0}
    for name, item in sorted(entries.items()):
        path = tree / name[1:]
        if not os.path.lexists(path):
            bad.append("missing in tree: " + name)
            continue
        st = os.lstat(path)
        if item["kind"] == "directory":
            counts["directories"] += 1
            if not stat.S_ISDIR(st.st_mode):
                bad.append("not a directory: " + name)
        elif item["kind"] == "symlink":
            counts["symlinks"] += 1
            if not stat.S_ISLNK(st.st_mode) or os.readlink(path) != item["target"]:
                bad.append("symlink differs: " + name)
        else:
            counts["files"] += 1
            if not stat.S_ISREG(st.st_mode) or st.st_size != item["size"]:
                bad.append("size/type differs: " + name)
            elif sha256_file(path) != item["sha256"]:
                bad.append("digest differs: " + name)
            want = 0o755 if item["mode"] & 0o111 else 0o644
            if stat.S_IMODE(st.st_mode) != want:
                bad.append("mode differs: %s %o != %o" % (name, stat.S_IMODE(st.st_mode), want))
    ancestors = {"/" + "/".join(n[1:].split("/")[:i]) for n in entries
                 for i in range(1, len(n[1:].split("/")))}
    extra = []
    for base, dirs, names in os.walk(tree):
        for n in dirs + names:
            rel = "/" + os.path.relpath(os.path.join(base, n), tree)
            if rel not in entries and rel not in ancestors:
                extra.append(rel)
    if extra:
        bad.append("extra in tree: %d (first: %s)" % (len(extra), extra[:3]))
    report["counts"] = counts
    # host drift
    drift = []
    for name, item in sorted(entries.items()):
        if item["kind"] != "file":
            continue
        if not os.path.isfile(name) or sha256_file(name) != item["sha256"]:
            drift.append(name)
    report["host_drift"] = drift
    # secrets heuristic by path only
    report["secret_like_paths"] = [n for n in entries
                                   if re.search(r"(\.key$|\.pem$|id_rsa|id_ed25519|\.p12$|\.pfx$|private[_-]?key)", n)]
    # ELF closure
    missing = {}
    interpreters = set()
    for name, item in sorted(entries.items()):
        if item["kind"] != "file":
            continue
        with open(tree / name[1:], "rb") as f:
            if f.read(4) != b"\x7fELF":
                continue
        r = subprocess.run(["ldd", name], capture_output=True, text=True)
        if r.returncode != 0:
            continue
        for line in r.stdout.splitlines():
            m = re.match(r"\s+(\S+) => (\S+) \(", line)
            if m and m.group(2) not in entries:
                real = os.path.realpath(m.group(2))
                if real not in entries:
                    missing.setdefault(m.group(2), []).append(name)
    report["ldd_libraries_not_in_seed"] = {k: v[:3] for k, v in sorted(missing.items())}
    report["interpreter_in_seed"] = "/libexec/ld-elf.so.1" in entries
    out = json.dumps(report, indent=1, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(out)
    print(out)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
