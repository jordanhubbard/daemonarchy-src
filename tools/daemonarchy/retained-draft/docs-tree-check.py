#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""DRAFT: whole-tree documentation check for a retained project (read-only, stdlib only).

Finds every documentation file selected by --include (glob, relative to --root, `**`
allowed), and checks each Markdown file for unreadable bytes (not UTF-8), relative links
and images whose target is missing from the tree, and `#fragment` links with no matching
heading (same slug rule as the repositories' docs-lifecycle tests: whitespace collapses,
so `### X -- Y` is `#x-y`; fragments compare case-insensitively like GitHub).  External URLs
are not fetched.  A per-file report is written to --output; the exit status is nonzero
when any file has a finding.  Only --include/--exclude select the tree; nothing is
inferred from the file system outside --root.

    docs-tree-check.py --root . --include 'docs/**/*.md' --include '*.md' --output report.json
"""
import argparse
import fnmatch
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING = re.compile(r"^ {0,3}#{1,6}[ \t]+(.+?)[ \t#]*$")


def slug(heading):
    heading = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    heading = heading.replace("`", "").replace("*", "").casefold()
    heading = re.sub(r"[^\w\-\s]", "", heading).strip()
    return re.sub(r"\s+", "-", heading)


def anchors(text):
    found, counts, fence = set(), {}, False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fence = not fence
            continue
        match = None if fence else HEADING.match(line)
        if match:
            base = slug(match.group(1))
            n = counts.get(base, 0)
            counts[base] = n + 1
            found.add(base if n == 0 else "%s-%d" % (base, n))
    return found


def matches(rel, patterns):
    for pat in patterns:
        regex = re.escape(pat).replace(r"\*\*/", "(?:.*/)?").replace(r"\*\*", ".*").replace(r"\*", "[^/]*").replace(r"\?", "[^/]")
        if re.fullmatch(regex, rel):
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".")
    ap.add_argument("--include", action="append", required=True)
    ap.add_argument("--exclude", action="append", default=[])
    ap.add_argument("--prune", action="append", default=[".git", "_build", "closure", "retained-out"],
                    help="directory names never entered")
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    root = Path(args.root).resolve()
    files = []
    for base, dirs, names in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in args.prune)
        for n in sorted(names):
            rel = os.path.relpath(os.path.join(base, n), root)
            if matches(rel, args.include) and not matches(rel, args.exclude):
                files.append(rel)
    cache = {}

    def headings(rel):
        if rel not in cache:
            try:
                cache[rel] = anchors((root / rel).read_text(encoding="utf-8"))
            except (OSError, UnicodeDecodeError):
                cache[rel] = set()
        return cache[rel]

    report, bad = [], 0
    for rel in files:
        path = root / rel
        entry = {"path": rel, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "findings": []}
        if rel.endswith(".md"):
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                entry["findings"].append("not valid UTF-8")
                text = ""
            fence = False
            for line in text.splitlines():
                if line.lstrip().startswith("```"):
                    fence = not fence
                    continue
                if fence:
                    continue
                for target in LINK.findall(line):
                    parts = urlsplit(target)
                    if parts.scheme or target.startswith("//") or target.startswith("mailto:"):
                        continue
                    dest = rel if not parts.path else os.path.normpath(
                        os.path.join(os.path.dirname(rel), unquote(parts.path)))
                    if dest.startswith(".."):
                        entry["findings"].append("link leaves the tree: " + target)
                        continue
                    if not (root / dest).exists():
                        entry["findings"].append("missing target: " + target)
                    elif parts.fragment and dest.endswith(".md") and \
                            unquote(parts.fragment).casefold() not in headings(dest):
                        entry["findings"].append("no such heading: " + target)
        bad += bool(entry["findings"])
        report.append(entry)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"schema": "daemonarchy/docs-tree-report@1", "include": args.include,
                               "exclude": args.exclude, "files": report}, indent=1) + "\n")
    print("%d documentation files, %d with findings" % (len(report), bad))
    for entry in report:
        for f in entry["findings"][:5]:
            print("  %s: %s" % (entry["path"], f), file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
