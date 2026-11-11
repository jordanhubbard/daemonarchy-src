#!/usr/bin/env python3
"""Check the mountroot console output ring with verbatim kernel code."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verbatim_fixture as vf  # noqa: E402

SOURCE = "sys/kern/daemonarchy_netconsole.c"
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-directory", type=Path, required=True)
args = parser.parse_args()
raise SystemExit(vf.run(
    "netconsole-output.c",
    [(SOURCE, "constants and console state",
      r"(?ms)^#define DNC_PORT .*?^static bool dnc_busy;\n"),
     (SOURCE, "dnc_flush", vf.function("dnc_flush", "static void")),
     (SOURCE, "dnc_putc", vf.function("dnc_putc", "static void"))],
    ["marker-survives", "overflow", "send-failure", "inactive"],
    "Verbatim mountroot console output ring and flush with a recording "
    "debugnet_send; not a kernel build or native console",
    args.output_directory,
    cflags=["-Wno-sign-compare", "-Wno-unused-variable", "-Wno-unused-parameter"]))
