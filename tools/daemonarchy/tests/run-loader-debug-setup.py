#!/usr/bin/env python3
"""Exercise loader Lua interactions; no EFI or physical NIC acceptance implied."""

import argparse
import json
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-directory", type=Path, required=True)
args = parser.parse_args()
out = args.output_directory.resolve()
out.mkdir(parents=True, exist_ok=False)
root = Path(__file__).resolve().parents[3]
lua = root / "contrib/lua/src"
# FreeBSD's build adds a local configuration header. This portable fixture
# deliberately omits platform extensions and builds only the bundled Lua VM.
(out / "luaconf.local.h").write_text("/* Portable controller fixture only. */\n")
sources = sorted(
    str(p) for p in lua.glob("*.c") if p.name not in ("luac.c", "ltests.c")
)
compile_argv = [
    os.environ.get("CC", "cc"),
    "-O0",
    "-DLUA_USE_POSIX",
    "-I" + str(out),
    "-I" + str(lua),
    *sources,
    "-lm",
    "-o",
    str(out / "lua"),
]
test_argv = [
    str(out / "lua"),
    str(root / "tools/daemonarchy/tests/loader-debug-setup.lua"),
    str(root),
]
records = []
for name, argv in (("compile", compile_argv), ("interactions", test_argv)):
    with (
        (out / (name + ".stdout")).open("xb") as stdout,
        (out / (name + ".stderr")).open("xb") as stderr,
    ):
        result = subprocess.run(
            argv, stdout=stdout, stderr=stderr, timeout=60, check=False
        )
    records.append({"phase": name, "argv": argv, "exit_code": result.returncode})
    (out / "result.json").write_text(
        json.dumps(
            {
                "scope": "portable Lua interaction fixture, not EFI acceptance",
                "phases": records,
            },
            indent=2,
        )
        + "\n"
    )
    if result.returncode:
        raise SystemExit(result.returncode)
print("loader debug setup: compilation and interaction checks passed")
