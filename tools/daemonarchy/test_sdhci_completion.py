#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Daemonarchy contributors
"""Exercise verbatim SDHCI completion functions with explicit service stubs.

This focused test does not qualify the kernel ABI, DMA hardware or a toolchain.
Select a retained original tree with --source for the regression control.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


def function(source, name):
    match = re.search(r"^(?:static )?void\n" + name + r"\([^;]*?\)\n\{", source, re.M)
    assert match, name
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def definitions(header, prefix):
    # Preserve literal definitions, including continuations; never mirror values.
    blocks = re.findall(r"^#define(?:[^\n]*?\\\n)*[^\n]*", header, re.M)
    return "\n".join(b for b in blocks if re.match(r"#define\s+(?:" + prefix + ")", b)) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    names = ["sys/dev/sdhci/sdhci.c", "sys/dev/sdhci/sdhci.h",
             "sys/dev/mmc/mmcreg.h", "sys/sys/bus_dma.h"]
    inputs = {n: (args.source / n).read_bytes() for n in names}
    source, header, mmc, dma = (inputs[n].decode() for n in names)
    notices = "\n".join(s.split("#include", 1)[0].split("#ifndef", 1)[0]
                        for s in [source, header, mmc, dma])
    constants = definitions(header, "SDHCI_INT_|SDHCI_(?:USE_DMA|PLATFORM_TRANSFER|RETUNE_REQ_RESET|SIGNAL_ENABLE|DMA_ADDRESS|RESET_CMD|RESET_DATA)\\b|PLATFORM_DATA_STARTED\\b")
    constants += definitions(mmc, "MMC_(?:ERR_|DATA_|RSP_BUSY\\b|TUNE_DONE\\b|SEND_TUNING_BLOCK)")
    constants += definitions(dma, "BUS_DMASYNC_")
    data_struct = re.search(r"^struct mmc_data \{.*?^\};", mmc, re.M | re.S).group()
    # Field types used by the real functions; layout and bus services are stubs.
    stubs = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u_char;
struct mmc_request { unsigned flags; };
struct mmc_command { struct mmc_data *data; uint32_t flags, opcode; int error; };
struct sdhci_slot {
    struct mmc_command *curcmd; struct mmc_request *req;
    size_t offset; uint32_t sdma_bbufsz, intmask, paddr;
    int flags, opt, cmd_done, data_done, retune_req, bus, dmatag, dmamap;
    void *dmamem;
};
static unsigned starts, restarts, signal_writes, resets, platform_finishes, pios;
static unsigned syncs[16];
static void write4(struct sdhci_slot *s, unsigned reg, uint32_t value) {
    (void)s; (void)value;
    if (reg == SDHCI_DMA_ADDRESS) restarts++;
    else if (reg == SDHCI_SIGNAL_ENABLE) signal_writes++;
    else abort();
}
#define WR4(s,r,v) write4(s,r,v)
#ifndef __predict_false
#define __predict_false(x) (x)
#endif
#define ulmin(a,b) ((a) < (b) ? (a) : (b))
#define slot_printf(...) ((void)0)
static void sdhci_dumpregs(struct sdhci_slot *s) { (void)s; }
static void bus_dmamap_sync(int tag, int map, unsigned op) {
    (void)tag; (void)map; if (op >= 16) abort(); syncs[op]++;
}
static void sdhci_start(struct sdhci_slot *s) { starts++; s->curcmd = NULL; }
static void sdhci_finish_command(struct sdhci_slot *s) { s->cmd_done = 1; }
static void sdhci_transfer_pio(struct sdhci_slot *s) { (void)s; pios++; }
#define SDHCI_RESET(b,s,f) ((void)(b), (void)(s), (void)(f), resets++)
#define SDHCI_PLATFORM_WILL_HANDLE(b,s) ((void)(b), (void)(s), 0)
#define SDHCI_PLATFORM_START_TRANSFER(b,s,m) ((void)(b), (void)(s), (void)(m))
#define SDHCI_PLATFORM_FINISH_TRANSFER(b,s) ((void)(b), (void)(s), platform_finishes++)
'''
    tests = Path(__file__).with_suffix('.c').read_text()
    generated = notices + '#include <stddef.h>\n#include <stdint.h>\n' + constants + data_struct + '\n' + stubs
    generated += function(source, 'sdhci_finish_data') + function(source, 'sdhci_data_irq') + tests
    cpath = args.output / 'production-excerpts.c'
    cpath.write_text(generated)
    binary = args.output / 'sdhci-completion-test'
    argv = shlex.split(args.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
    if args.sanitize:
        argv += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    argv += [str(cpath), '-o', str(binary)]
    result = {'scope': __doc__, 'inputs': {n: hashlib.sha256(b).hexdigest() for n, b in inputs.items()}, 'compile_argv': argv}
    build = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    (args.output / 'compile.stdout').write_text(build.stdout)
    (args.output / 'compile.stderr').write_text(build.stderr)
    result['compile_exit'] = build.returncode
    if build.returncode == 0:
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        (args.output / 'test.stdout').write_text(run.stdout)
        (args.output / 'test.stderr').write_text(run.stderr)
        result.update(test_exit=run.returncode, passed=run.stdout.count('PASS '), failed=run.stdout.count('FAIL '))
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return build.returncode or result['test_exit']


if __name__ == '__main__':
    raise SystemExit(main())
