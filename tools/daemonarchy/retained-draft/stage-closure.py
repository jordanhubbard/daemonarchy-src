#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""DRAFT: copy the observed closure into the untracked closure/ staging directory.

Reads closure-inventory.json (see closure-inventory.py), copies each tool, library and
data tree to closure/<role>/{bin,lib,<dest>} under the project root, and verifies every
copied file against the inventory digest.  It refuses to overwrite a file whose bytes
differ.  It runs nothing it copies and changes nothing outside closure/.

    python3.12 stage-closure.py --inventory closure-inventory.json --root .
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def put(src, dst, digest, mode):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        if sha256_file(dst) != digest:
            raise SystemExit("refusing to overwrite differing file: %s" % dst)
        return
    shutil.copyfile(src, dst)
    os.chmod(dst, mode)
    if sha256_file(dst) != digest:
        raise SystemExit("copy does not match inventory (host file changed?): %s" % src)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--inventory", required=True)
    ap.add_argument("--root", required=True)
    args = ap.parse_args()
    inv = json.loads(Path(args.inventory).read_text())
    if inv["problems"]:
        raise SystemExit("inventory has unresolved problems: %s" % inv["problems"])
    root = Path(args.root).resolve() / "closure"
    for t in inv["tools"]:
        put(t["realpath"], root / t["role"] / "bin" / t["name"], t["sha256"], 0o755)
    for lib in inv["libraries"]:
        put(lib["realpath"], root / lib["role"] / "lib" / lib["soname"], lib["sha256"], 0o755)
    for tree in inv["trees"]:
        excluded = set(tree["exclude"])
        dest = root / tree["role"] / tree["dest"]
        for base, dirs, names in os.walk(tree["source"]):
            dirs[:] = sorted(d for d in dirs if d not in excluded)
            rel = os.path.relpath(base, tree["source"])
            for d in dirs:
                (dest / rel / d).mkdir(parents=True, exist_ok=True)
            for n in sorted(names):
                src = Path(base) / n
                out = dest / rel / n
                out.parent.mkdir(parents=True, exist_ok=True)
                # File symlinks are captured as regular files holding the target's bytes
                # (the stdlib links lib-dynload/_sqlite3 into site-packages, which is
                # not captured); os.stat/copyfile follow the link.
                mode = 0o755 if os.stat(src).st_mode & 0o111 else 0o644
                put(src, out, sha256_file(src), mode)
    print(json.dumps({"staged_under": str(root)}))


if __name__ == "__main__":
    sys.exit(main())
