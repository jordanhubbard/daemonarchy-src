#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""DRAFT: observe the real tool, library and data closure a retained action set needs.

Read-only.  It reads the files named by a closure spec, hashes them, asks ldd(1) and
pkg(8) about them, and writes a deterministic inventory (no timestamps, sorted keys).
It executes no project code and asserts no build, test or acceptance result.  Run it on
the FreeBSD worker the retained run will use; the inventory binds that host's bytes.

    python3.12 closure-inventory.py --spec closure-spec.json --output closure-inventory.json

Spec (JSON):
  search     ordered directories used to resolve a bare command name
  tools      [{name, command | path, role: toolchain|dependency, why}]
  trees      [{name, source, dest, role, exclude: [dir names], scan_libs: bool, why}]
  ambient    [{name, path, kind: file|tree|listing, why}]  host files that cannot be
             relocated into the captured tree (absolute paths); recorded by identity so
             a preflight can refuse drift.  Anything not listed here is UNKNOWN.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

LOADER = "/libexec/ld-elf.so.1"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def run(argv):
    r = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout, r.stderr


_owner_cache = {}


def owner(path):
    real = os.path.realpath(path)
    if real in _owner_cache:
        return _owner_cache[real]
    rc, out, _ = run(["pkg", "which", "-q", real])
    if rc == 0 and out.strip():
        rc2, out2, _ = run(["pkg", "query", "%n-%v %o", out.strip().split()[0]])
        name = out.strip().split()[0]
        value = {"kind": "pkg", "name": name}
        if rc2 == 0 and out2.strip():
            value["version_origin"] = out2.strip()
    elif real.startswith("/usr/local/"):
        value = {"kind": "unowned-local"}
    else:
        value = {"kind": "base-system"}
    _owner_cache[real] = value
    return value


def describe(path):
    real = os.path.realpath(path)
    st = os.stat(real)
    return {
        "path": path,
        "realpath": real,
        "size": st.st_size,
        "mode": "0%o" % (st.st_mode & 0o7777),
        "sha256": sha256_file(real),
        "owner": owner(real),
    }


def libraries(path):
    """Resolve the ldd(1) closure of one ELF file: {soname: path}."""
    rc, out, err = run(["ldd", os.path.realpath(path)])
    if rc != 0:
        return None
    found = {}
    for line in out.splitlines():
        m = re.match(r"\s+(\S+) => (\S+) \(0x[0-9a-f]+\)", line)
        if m:
            found[m.group(1)] = m.group(2)
        elif re.match(r"\s+(\S+) => not found", line):
            found[line.split()[0]] = None
    return found


def shebang(path):
    with open(path, "rb") as f:
        head = f.read(256)
    if head.startswith(b"#!"):
        line = head.split(b"\n", 1)[0][2:].decode("utf-8", "replace").strip()
        return line
    return None


def tree_digest(source, exclude):
    h = hashlib.sha256()
    files = nbytes = 0
    members = []
    for base, dirs, names in os.walk(source):
        dirs[:] = sorted(d for d in dirs if d not in exclude)
        for d in dirs:
            if os.path.islink(os.path.join(base, d)):
                raise SystemExit("directory symlink inside a captured tree: " + os.path.join(base, d))
        for name in sorted(names):
            full = os.path.join(base, name)
            rel = os.path.relpath(full, source)
            if os.path.islink(full) and not os.path.isfile(full):
                raise SystemExit("symlink to a non-file inside a captured tree: " + full)
            st = os.stat(full)
            digest = sha256_file(full)
            h.update(b"F\0" + rel.encode() + b"\0" + str(st.st_size).encode() + b"\0" +
                     ("%o" % (st.st_mode & 0o111 and 0o755 or 0o644)).encode() + b"\0" +
                     digest.encode() + b"\n")
            members.append(full)
            files += 1
            nbytes += st.st_size
    return {"files": files, "bytes": nbytes, "digest": h.hexdigest()}, members


def elf_candidates(members):
    for full in members:
        if os.path.islink(full):
            continue
        if ".so" in os.path.basename(full):
            yield full


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--spec", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    spec = json.loads(Path(args.spec).read_text())
    search = spec["search"]
    problems = []
    tools, libs = [], {}

    def add_libs(consumer, role, found):
        if found is None:
            problems.append("ldd failed or not a dynamic ELF: " + consumer)
            return []
        names = []
        for soname, path in sorted(found.items()):
            if path is None:
                problems.append("%s: %s not found" % (consumer, soname))
                continue
            key = (role, soname)
            real = os.path.realpath(path)
            if key in libs and libs[key]["realpath"] != real:
                problems.append("soname collision %s: %s vs %s" % (soname, libs[key]["realpath"], real))
                continue
            if key not in libs:
                libs[key] = dict(describe(path), soname=soname, role=role)
            names.append(soname)
        return names

    for t in spec["tools"]:
        role = t.get("role", "toolchain")
        if "path" in t:
            resolved = t["path"]
        else:
            resolved = next((os.path.join(d, t["command"]) for d in search
                             if os.access(os.path.join(d, t["command"]), os.X_OK)), None)
        if resolved is None or not os.path.isfile(resolved):
            problems.append("tool not found: " + t["name"])
            continue
        entry = dict(describe(resolved), name=t["name"], role=role, why=t.get("why", ""))
        if "env" in t:
            entry["env"] = t["env"]
        she = shebang(os.path.realpath(resolved))
        if she:
            entry["interpreter"] = she
            entry["libraries"] = []
        else:
            entry["libraries"] = add_libs(t["name"], role, libraries(resolved))
        tools.append(entry)

    trees = []
    for tr in spec.get("trees", []):
        role = tr.get("role", "toolchain")
        info, members = tree_digest(tr["source"], set(tr.get("exclude", [])))
        entry = {"name": tr["name"], "source": tr["source"], "dest": tr["dest"], "role": role,
                 "exclude": sorted(tr.get("exclude", [])), "why": tr.get("why", ""), **info,
                 "owner": owner(members[0]) if members else None}
        if tr.get("scan_libs"):
            for so in elf_candidates(members):
                found = libraries(so)
                if found:
                    inside = {k: v for k, v in found.items()
                              if v is None or not os.path.realpath(v).startswith(tr["source"] + "/")}
                    add_libs(os.path.relpath(so, tr["source"]), role, inside)
        trees.append(entry)

    ambient = []
    for a in spec.get("ambient", []):
        p = a["path"]
        rec = {"name": a["name"], "path": p, "kind": a["kind"], "why": a.get("why", "")}
        if not os.path.lexists(p):
            rec["state"] = "absent"
            problems.append("ambient input absent: " + p)
        elif a["kind"] == "file":
            rec.update(describe(p))
            rec["path"] = p
        elif a["kind"] == "tree":
            info, _ = tree_digest(p, set(a.get("exclude", [])))
            rec.update(info)
            rec["exclude"] = sorted(a.get("exclude", []))
        elif a["kind"] == "listing":
            names = sorted(os.listdir(p))
            h = hashlib.sha256()
            for n in names:
                full = os.path.join(p, n)
                st = os.lstat(full)
                h.update(("%s\0%d\n" % (n, st.st_size)).encode())
            rec.update({"entries": len(names), "digest": h.hexdigest()})
        ambient.append(rec)

    ambient.append(dict(describe(LOADER), name="dynamic-linker", kind="file", path=LOADER,
                        why="ELF interpreter of every captured binary; its absolute path is "
                            "recorded in the executable and cannot be relocated"))
    for t in tools:
        she = t.get("interpreter")
        if she:
            interp = she.split()[0]
            if os.path.isfile(interp) and not any(a["path"] == interp for a in ambient):
                ambient.append(dict(describe(interp), name="interpreter:" + os.path.basename(interp),
                                    kind="file", path=interp,
                                    why="shebang of captured script " + t["name"]))

    _, ver_k, _ = run(["freebsd-version", "-k"])
    _, ver_u, _ = run(["freebsd-version", "-u"])
    _, machine, _ = run(["uname", "-m"])
    inventory = {
        "schema": "daemonarchy/retained-closure-inventory@1",
        "host": {"freebsd_kernel": ver_k.strip(), "freebsd_userland": ver_u.strip(),
                 "machine": machine.strip()},
        "spec_sha256": hashlib.sha256(Path(args.spec).read_bytes()).hexdigest(),
        "tools": sorted(tools, key=lambda t: t["name"]),
        "libraries": [libs[k] for k in sorted(libs)],
        "trees": trees,
        "ambient": ambient,
        "problems": sorted(problems),
    }
    Path(args.output).write_text(json.dumps(inventory, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"tools": len(tools), "libraries": len(libs), "trees": len(trees),
                      "ambient": len(ambient), "problems": sorted(problems)}, indent=1))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
