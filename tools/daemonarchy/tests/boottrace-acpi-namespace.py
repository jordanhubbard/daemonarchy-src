#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile the original bounded ACPI reader and verbatim loader slot guard.

Use a new --work-dir. These controls test firmware-memory service stubs, not
actual firmware memory admission, block writes, AML or a complete EFI loader.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=False)
    source = ROOT / "stand/efi/loader"
    fixture = Path(__file__).with_suffix(".c")
    loader = (source / "boottrace.c").read_text()
    start = loader.index("static bool\nbt_acpi_namespace_slots(void)\n")
    end = loader.index("\n}\n", start) + 3
    function = loader[start:end]
    (args.work_dir / "namespace-slots-function.h").write_text(function)
    compiler = Path(shutil.which(args.cc) or args.cc).resolve(strict=True)
    files = [source / "boottrace.c", source / "boottrace_acpi.c",
             source / "boottrace_acpi.h", fixture, Path(__file__)]
    identities = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in files}
    result = {"scope": __doc__, "input_sha256": identities,
              "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "commands": []}
    def run(name, argv):
        completed = subprocess.run(argv, capture_output=True, text=True)
        (args.work_dir / (name + ".stdout")).write_text(completed.stdout)
        (args.work_dir / (name + ".stderr")).write_text(completed.stderr)
        result["commands"].append(dict(name=name, argv=[str(a) for a in argv],
                                       exit=completed.returncode))
        (args.work_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if completed.returncode:
            raise RuntimeError(f"{name} failed; original output retained")
        return completed.stdout
    run("compiler-version", [str(compiler), "--version"])
    binary = args.work_dir / "controls"
    argv = [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-g", "-O1",
            "-I", str(source), "-I", str(args.work_dir)]
    if args.sanitize:
        argv += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    run("compile", argv + [str(source / "boottrace_acpi.c"), str(fixture), "-o", str(binary)])
    output = run("controls", [str(binary)])
    assert re.fullmatch(r"24 namespace reader and legacy-media guard cases passed\n", output), output
    assert identities == {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in files}
    result.update(input_bytes_unchanged=True, cases=24, sanitized=args.sanitize,
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    (args.work_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(output, end="")


if __name__ == "__main__":
    main()
