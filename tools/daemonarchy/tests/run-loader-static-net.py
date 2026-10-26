#!/usr/bin/env python3
"""Check static loader network parameters and the TFTP port with verbatim code."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import verbatim_fixture as vf  # noqa: E402

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-directory", type=Path, required=True)
args = parser.parse_args()
raise SystemExit(vf.run(
    "loader-static-net.c",
    [("stand/common/dev_net.c", "uri_schemes",
      r"(?ms)^static struct uri_scheme \{.*?^\};\n"),
     ("stand/common/dev_net.c", "net_parse_rootpath",
      vf.function("net_parse_rootpath", "uint32_t")),
     ("stand/common/dev_net.c", "net_static_params",
      vf.function("net_static_params", "static int")),
     ("stand/libsa/tftp.c", "tftp_server_port",
      vf.function("tftp_server_port", "static unsigned short"))],
    ["tftp-port", "defaults", "not-configured", "refused"],
    "Verbatim loader static network parameters and TFTP port selection; "
    "not an EFI loader build or a native TFTP transfer",
    args.output_directory,
    cflags=["-Wno-sign-compare", "-Wno-unused-parameter"]))
