#!/usr/bin/env python3
"""Walk verbatim production BIOS tables; not native firmware acceptance."""
import argparse
from pathlib import Path
import verbatim_fixture as vf
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output-directory', type=Path, required=True)
args = parser.parse_args()
raise SystemExit(vf.run('amd64-bootmap.c',
    [('stand/i386/libi386/elf64_freebsd.c', 'amd64_bootmap_init',
      vf.function('amd64_bootmap_init', 'static int'))],
    ['boundary', 'legacy-alias', 'bounds'],
    'Verbatim production table construction, independent x86 page walker; not firmware or kernel execution',
    args.output_directory))
