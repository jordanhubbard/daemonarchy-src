#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise actual MSI registry functions with injected mutex/allocation primitives.

This host control proves query selection, locking and absence of allocation or
registry mutation. It does not prove native interrupt, provider hardware or
post-query lifetime behavior.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "sys/kern/subr_intr.c"
HEADER = ROOT / "sys/sys/intr.h"


def function(source, name):
    match = re.search(r"\n(?:static(?: inline)? )?[\w *]+\n" + name + r"\(", source)
    if match is None:
        raise ValueError(name)
    begin = match.start() + 1
    pos = source.index("{", match.end())
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end] + "\n"


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
typedef unsigned u_int;
struct device { unsigned identity; };
typedef struct device *device_t;
struct mtx { bool initialized, held; };
#define MA_OWNED 1
#define MTX_SPIN 2
#define M_INTRNG 0
#define M_NOWAIT 1
#define M_ZERO 2
#define debugf(...) ((void)0)
'''
SERVICES = r'''
static struct mtx pic_list_lock = { .initialized = true };
static SLIST_HEAD(, intr_pic) pic_list;
static unsigned allocations, locks, unlocks;
static bool fail_allocation;
static void (*before_lock)(void);
static void mtx_assert(struct mtx *m, int owned) {
 assert(m->initialized && m->held && owned == MA_OWNED);
}
static void mtx_lock(struct mtx *m) {
 if (before_lock != NULL) { void (*hook)(void) = before_lock; before_lock = NULL; hook(); }
 assert(m->initialized && !m->held); m->held = true; locks++;
}
static void mtx_unlock(struct mtx *m) {
 assert(m->initialized && m->held); m->held = false; unlocks++;
}
static void mtx_init(struct mtx *m, const char *name, void *arg, int flags) {
 (void)name; (void)arg; assert(flags == MTX_SPIN); m->initialized = true;
}
static void *host_malloc(size_t length, int flags) {
 assert(pic_list_lock.held && flags == (M_NOWAIT | M_ZERO));
 if (fail_allocation) return NULL;
 allocations++;
 return calloc(1, length);
}
#define malloc(length, type, flags) host_malloc(length, flags)
'''
TEST = r'''
static struct device a = {1}, b = {2}, c = {3};
static unsigned cases;
static void reset(void) {
 struct intr_pic *pic;
 assert(!pic_list_lock.held);
 while ((pic = SLIST_FIRST(&pic_list)) != NULL) {
  SLIST_REMOVE_HEAD(&pic_list, pic_next); free(pic);
 }
 allocations = locks = unlocks = 0;
 fail_allocation = false; before_lock = NULL;
}
static void check(device_t expected, intptr_t xref, int error) {
 struct intr_pic *items[16]; intptr_t xrefs[16]; device_t devices[16]; u_int flags[16];
 struct intr_pic *pic; unsigned count = 0, index = 0, old_allocations = allocations;
 SLIST_FOREACH(pic, &pic_list, pic_next) {
  assert(count < 16); items[count] = pic; xrefs[count] = pic->pic_xref;
  devices[count] = pic->pic_dev; flags[count++] = pic->pic_flags;
 }
 unsigned old_locks = locks;
 assert(intr_msi_validate_provider(expected, xref) == error);
 assert(!pic_list_lock.held && allocations == old_allocations && locks == unlocks);
 assert(locks == old_locks + (expected != NULL && xref != 0));
 SLIST_FOREACH(pic, &pic_list, pic_next) {
  assert(index < count && pic == items[index] && pic->pic_xref == xrefs[index] &&
   pic->pic_dev == devices[index] && pic->pic_flags == flags[index]); index++;
 }
 assert(index == count); cases++;
}
static void inject_duplicate(device_t dev, intptr_t xref) {
 struct intr_pic *pic = calloc(1, sizeof(*pic)); assert(pic != NULL);
 pic->pic_dev = dev; pic->pic_xref = xref; pic->pic_flags = FLAG_MSI;
 SLIST_INSERT_HEAD(&pic_list, pic, pic_next);
}
static void publish_before_lock(void) { assert(intr_msi_register(&a, 39) == 0); }
int main(void) {
 reset(); check(NULL, 39, EINVAL); check(&a, 0, EINVAL); check(NULL, 0, EINVAL);
 check(&a, 39, ESRCH); /* Initialized cold registry. */
 assert(pic_create(&a, 39, FLAG_PIC) != NULL); check(&a, 39, ESRCH);
 reset(); assert(intr_msi_register(&b, 39) == 0); check(&a, 39, ENXIO);
 check(&b, 40, ESRCH); check(&b, 39, 0);
 reset(); assert(intr_msi_register(&a, 0) == 0); check(&a, 39, ESRCH);
 /* Actual pic_create treats existing device-only registration as a wildcard.
  * This must never masquerade as the exact xref allocation resolves. */
 assert(intr_msi_register(&a, 39) == 0 && allocations == 1); check(&a, 39, ESRCH);
 reset(); assert(intr_msi_register(&a, 39) == 0); unsigned count = allocations;
 assert(intr_msi_register(&a, 39) == 0 && allocations == count); check(&a, 39, 0);
 /* Same-device injected duplication is corruption, not normal idempotence. */
 inject_duplicate(&a, 39); check(&a, 39, EEXIST);
 reset(); assert(intr_msi_register(&a, 39) == 0);
 assert(intr_msi_register(&b, 39) == 0); check(&a, 39, EEXIST); check(&b, 39, EEXIST);
 reset(); assert(intr_msi_register(&b, 39) == 0);
 assert(intr_msi_register(&a, 39) == 0); check(&a, 39, EEXIST); check(&b, 39, EEXIST);
 reset(); assert(intr_msi_register(&a, 39) == 0);
 assert(pic_create(&b, 39, FLAG_PIC) != NULL);
 assert(intr_msi_register(&c, 40) == 0); check(&a, 39, 0); check(&c, 40, 0);
 reset(); assert(intr_msi_register(&a, 39) == 0);
 /* Device-only wildcard belonging to somebody else is not an exact alias. */
 assert(intr_msi_register(&b, 0) == 0); check(&a, 39, 0); check(&b, 39, ENXIO);
 reset(); fail_allocation = true; assert(intr_msi_register(&a, 39) == ENOMEM);
 check(&a, 39, ESRCH);
 reset(); before_lock = publish_before_lock;
 /* Publication happens immediately before lock acquisition: snapshot must see it. */
 assert(intr_msi_validate_provider(&a, 39) == 0 && locks == unlocks && !pic_list_lock.held); cases++;
 reset(); printf("MSI provider query production controls PASS cases=%u\n", cases);
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="msi-provider-query-"))
    out.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text()
    assert "int intr_msi_validate_provider(device_t, intptr_t);" in HEADER.read_text()
    begin = source.index("struct intr_pic {\n")
    end = source.index("\n};", begin) + len("\n};")
    functions = "\n".join(function(source, name) for name in [
        "pic_lookup_locked", "pic_create", "intr_msi_register", "intr_msi_validate_provider"
    ])
    generated = PREFIX + source[begin:end] + SERVICES + functions + TEST
    (out / "control.c").write_text(generated)
    command = [args.cc, "-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               str(out / "control.c"), "-o", str(out / "control")]
    subprocess.run(command, check=True)
    run = subprocess.run([str(out / "control")], check=True, text=True, capture_output=True)
    print(run.stdout, end="")
    (out / "run.log").write_text(run.stdout + run.stderr)
    result = {"ok": True, "scope": "host production registry functions; injected mutex/allocation primitives",
              "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              "header_sha256": hashlib.sha256(HEADER.read_bytes()).hexdigest(),
              "test_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "generated_sha256": hashlib.sha256(generated.encode()).hexdigest(),
              "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
              "compiler_path": str(Path(shutil.which(args.cc)).resolve()),
              "compiler_sha256": hashlib.sha256(Path(shutil.which(args.cc)).read_bytes()).hexdigest(),
              "command": command,
              "binary_sha256": hashlib.sha256((out / "control").read_bytes()).hexdigest()}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
