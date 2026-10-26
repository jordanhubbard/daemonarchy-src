#!/usr/bin/env python3
"""Check TFTP whole-file preload with the verbatim libsa read, seek and preload."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verbatim_fixture as vf  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-directory", type=Path, required=True)
args = parser.parse_args()
raise SystemExit(vf.run(
    "tftp-preload.c",
    [("stand/libsa/tftp.c", "struct tftp_handle and cache hooks",
      r"(?ms)^struct tftp_handle \{.*?^void \(\*tftp_cache_free\)\(void \*, size_t\);\n"),
     ("stand/libsa/tftp.c", "tftp_cache_release",
      vf.function("tftp_cache_release", "static void")),
     ("stand/libsa/tftp.c", "tftp_read", vf.function("tftp_read", "static int")),
     ("stand/libsa/tftp.c", "tftp_close", vf.function("tftp_close", "static int")),
     ("stand/libsa/tftp.c", "tftp_seek", vf.function("tftp_seek", "static off_t")),
     ("stand/libsa/tftp.c", "tftp_preload", vf.function("tftp_preload", "static int"))],
    ["platform-cache", "heap-fallback", "restart-once", "no-size", "oversize"],
    "Verbatim libsa TFTP read, seek, close and whole-file preload against a "
    "simulated server; not an EFI loader build or a native transfer",
    args.output_directory,
    cflags=["-Wno-sign-compare", "-Wno-unused-parameter", "-Wno-unused-function"]))
