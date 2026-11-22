#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Daemonarchy contributors
"""Compile verbatim CMD5 sender/completion excerpts with bounded service stubs.

No controller or radio is emulated. A full native kernel build remains necessary.
--source can select retained original inputs for counterfactual regression runs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


def function(source, name):
    match = re.search(r"(?:static (?:inline )?\w+\n)" + name + r"\([^;]*?\)\n\{", source)
    if match is None:
        return ""
    start = match.start()
    pos = source.index("{", match.start())
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def case(source, name):
    body = function(source, name)
    assert body, name
    return body.split("\tcase PROBE_SDIO_INIT:", 1)[1].split("\n\tcase ", 1)[0]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--cc", default="cc")
    p.add_argument("--sanitize", action="store_true")
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    paths = ["sys/cam/mmc/mmc_xpt.c", "sys/dev/mmc/mmcreg.h", "sys/cam/mmc/mmc.h"]
    inputs = {name: (args.source / name).read_bytes() for name in paths}
    source, header, feature_header = (inputs[name].decode() for name in paths)
    enum = re.search(r"typedef enum \{.*?\} probe_action;", source, re.S).group()
    debug = source.split("static char *probe_action_text[]", 1)[1].split("static struct xpt_xport_ops", 1)[0]
    definitions = "\n".join(line for line in header.splitlines() if re.match(
        r"#define\s+(?:R4_|MMC_RSP_|MMC_CMD_|MMC_ERR_|IO_SEND_OP_COND)", line))
    definitions += "\n" + "\n".join(line for line in feature_header.splitlines() if re.match(r"#define\s+CARD_FEATURE_", line))
    limits = "\n".join(line for line in source.splitlines() if re.match(r"#define\s+SDIO_INIT_", line))
    # Explicit service/type stubs; these do not claim kernel ABI/layout coverage.
    stubs = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef unsigned int u_int;
typedef int64_t sbintime_t;
#define SBT_1S ((sbintime_t)1 << 32)
#define CAM_STATUS_MASK 0x3f
#define CAM_REQ_CMP 1
#define CAM_DIR_OUT 0x80
#define XPT_MMC_IO 0x91d
#define CAM_DEBUG_PROBE 1
#define OTHER_FEATURES 0x1bu
static sbintime_t now;
static unsigned delays, debug_calls;
sbintime_t sbinuptime(void) { return now; }
void delay(unsigned us) { if (us != 1000) __builtin_trap(); delays++; }
#define DELAY(us) delay(us)
static void debug_sink(const char *fmt, ...) { (void)fmt; debug_calls++; }
/* Evaluate debug operands, exercising PROBE_SET_ACTION's repeated expression. */
#define CAM_DEBUG(path, flag, args) do { (void)(path); (void)(flag); debug_sink args; } while (0)
struct mmc_params { uint32_t io_ocr, card_features; uint8_t sdio_func_count; };
struct device { struct mmc_params mmc_ident_data; };
struct cam_path { struct device *device; };
struct cam_periph { struct cam_path *path; };
union ccb;
struct ccb_hdr { unsigned status, func_code, flags, retry_count, timeout;
    void (*cbfcnp)(struct cam_periph *, union ccb *); struct cam_path *path; };
struct command { uint32_t opcode, arg, resp[4], flags, error; };
struct ccb_mmcio { struct ccb_hdr ccb_h; struct command cmd, stop; };
union ccb { struct ccb_hdr ccb_h; struct ccb_mmcio mmcio; };
static void mmcprobe_done(struct cam_periph *p, union ccb *c) { (void)p; (void)c; }
'''
    softc = r'''
typedef struct { probe_action action; int restart; uint32_t host_ocr, flags;
    sbintime_t sdio_started; unsigned sdio_tries; struct cam_periph *periph;
} mmcprobe_softc;
'''
    sender = '''static void send_cmd(mmcprobe_softc *softc, struct mmc_params *mmcp, union ccb *start_ccb) {
struct ccb_mmcio *mmcio = &start_ccb->mmcio;
switch (softc->action) { case PROBE_SDIO_INIT:
''' + case(source, "mmcprobe_start") + '\ndefault: break; }\n}\n'
    completion = '''static void complete_cmd(mmcprobe_softc *softc, struct cam_path *path, union ccb *done_ccb) {
struct ccb_mmcio *mmcio = &done_ccb->mmcio; int err = 0; (void)mmcio; (void)err;
switch (softc->action) { case PROBE_SDIO_INIT:
''' + case(source, "mmcprobe_done") + '\ndefault: break; }\n}\n'
    restart = function(source, "mmcprobe_sdio_restart")
    if restart:
        restart += '#define HAVE_RESTART_HELPER 1\n'
    tests = Path(__file__).with_suffix(".c").read_text()
    notices = source.split('#include', 1)[0] + header.split('#ifndef DEV_MMC_MMCREG_H', 1)[0] + feature_header.split('#ifndef', 1)[0]
    generated = notices + stubs + enum + '\nstatic char *probe_action_text[]' + debug + '\n' + definitions + '\n' + limits + '\n' + softc
    generated += function(source, 'init_standard_ccb') + restart + function(source, 'mmcprobe_sdio_init_done') + sender + completion + tests
    cpath = args.output / 'production-excerpts.c'
    cpath.write_text(generated)
    binary = args.output / 'sdio-init-test'
    argv = shlex.split(args.cc) + ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror']
    if args.sanitize:
        argv += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    argv += [str(cpath), '-o', str(binary)]
    results = {'scope': 'Verbatim production CMD5 sender/completion and restart helper; explicit CAM/time/delay/type stubs; no kernel ABI, controller or radio qualification.',
               'inputs': {k: hashlib.sha256(v).hexdigest() for k, v in inputs.items()}, 'compile_argv': argv}
    build = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    for name, value in [('compile.stdout', build.stdout), ('compile.stderr', build.stderr)]:
        (args.output / name).write_text(value)
    results['compile_exit'] = build.returncode
    if build.returncode == 0:
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        (args.output / 'test.stdout').write_text(run.stdout)
        (args.output / 'test.stderr').write_text(run.stderr)
        results.update(test_exit=run.returncode, passed=run.stdout.count('PASS '), failed=run.stdout.count('FAIL '))
    (args.output / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(results, indent=2))
    return build.returncode or results['test_exit']


if __name__ == '__main__':
    raise SystemExit(main())
