#!/usr/bin/env python3
"""Check debugnet duplicate-input suppression with the verbatim receive path."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verbatim_fixture as vf  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-directory", type=Path, required=True)
args = parser.parse_args()
raise SystemExit(vf.run(
    "debugnet-rx-duplicate.c",
    [("sys/net/debugnet.c", "debugnet_rx_digest",
      vf.function("debugnet_rx_digest", "static uint32_t")),
     ("sys/net/debugnet.c", "debugnet_rx_is_duplicate",
      vf.function("debugnet_rx_is_duplicate", "static bool")),
     ("sys/net/debugnet.c", "debugnet_handle_rx_msg",
      vf.function("debugnet_handle_rx_msg", "static void"))],
    ["lost-ack", "restart-reuse", "refused", "chained", "new-session", "finish-short"],
    "Verbatim debugnet receive path with a NetGDB-like appending consumer; "
    "not a linked kernel or native NetGDB session",
    args.output_directory,
    # The kernel build does not enable -Wsign-compare for this file.
    cflags=["-Wno-sign-compare"]))
