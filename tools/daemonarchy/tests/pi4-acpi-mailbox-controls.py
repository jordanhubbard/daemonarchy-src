#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Fault-inject verbatim production BCM2711 mailbox functions.

These userspace service stubs test resource refusal, polling bounds and DMA
ownership. They do not emulate VideoCore, qualify real DMA mappings, or replace
native kernel compilation and physical mailbox acceptance. Retain generated
source, binaries and all command output in a new --work-dir.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
BASE = ROOT / "sys/arm/broadcom/bcm2835"


def function(path, name):
    source = path.read_text()
    match = re.search(r"(?m)^(?:static )?(?:int|void)\n" + name + r"\(", source)
    if match is None:
        raise ValueError(f"missing production function: {name}")
    start = match.start()
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned u_int;
typedef uint64_t bus_addr_t;
typedef void *device_t;
#define DEV ((device_t)(uintptr_t)1)
#define PARENT ((device_t)(uintptr_t)2)
typedef int bus_dma_tag_t;
typedef int bus_dmamap_t;
typedef struct { bus_addr_t ds_addr; } bus_dma_segment_t;
#define BCM2838_ARM_IO_BASE UINT64_C(0xfe000000)
#define SYS_RES_MEMORY 1
#define REG_WRITE 0x20
#define REG_STATUS 0x18
#define STATUS_EMPTY 0x40000000
#define BUS_PROBE_DEFAULT 0
#define BCM2835_MBOX_CHAN_PROP 8
#define BUS_DMASYNC_PREWRITE 1
#define BUS_DMASYNC_PREREAD 2
#define BUS_DMASYNC_POSTWRITE 4
#define BUS_DMASYNC_POSTREAD 8
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define ARMC_TO_VCBUS(a) (a)
#define dprintf(...) ((void)0)
#define MBOX_DATA(a) ((a) & ~15U)
struct bcm_mbox_softc {
 bool acpi, polling, failed;
 int property_chan_lock, lock;
 int have_message[16];
 uint32_t msg[16];
};
struct bcm_mbox_dma_result { uint64_t address; int error; };
struct bcm2835_mbox_hdr { uint32_t dummy; };
struct bcm283x_memory_soc_cfg { const char *soc_compat; };
static struct bcm283x_memory_soc_cfg bcm283x_memory_configs[] = {
 {"brcm,bcm2835"}, {"brcm,bcm2711"}, {"brcm,bcm2712"}
};
static struct bcm283x_memory_soc_cfg *booted_soc_memcfg;
static struct bcm_mbox_softc sc;
static int available = 1, attached = 1, disabled, id_error;
static bus_addr_t memory_start = UINT64_C(0xfe00b880), memory_count = 0x24;
static int descriptions, allocations, allocation_fails, writes, reads;
static int write_error, read_error, format_error, validations, frees, unloads, destroys;
static int syncs[4], sync_count;
static uint32_t address = 0xc0001000, response_address = 0xc0001000;
static unsigned char buffer[64];
static int cold, hz = 100, empty_fifo, foreign_channel, poll_error;
static unsigned delays, polls, fifo_reads;
static void *device_get_softc(device_t d) { assert(d == DEV); return &sc; }
static int acpi_disabled(const char *s) { assert(!strcmp(s,"mbox")); return disabled; }
static device_t device_get_parent(device_t d) { assert(d == DEV); return PARENT; }
static int id_probe(device_t p, device_t d, char **ids, void *arg) {
 assert(p == PARENT && d == DEV && !strcmp(ids[0],"BCM2849") && !ids[1] && !arg);
 return id_error;
}
#define ACPI_ID_PROBE id_probe
static bus_addr_t bus_get_resource_start(device_t d, int type, int rid) {
 assert(d == DEV && type == SYS_RES_MEMORY && rid == 0); return memory_start;
}
static bus_addr_t bus_get_resource_count(device_t d, int type, int rid) {
 assert(d == DEV && type == SYS_RES_MEMORY && rid == 0); return memory_count;
}
static void device_set_desc(device_t d, const char *s) {
 assert(d == DEV && s && *s); descriptions++;
}
static int devclass_find(const char *s) { assert(!strcmp(s,"mbox")); return 1; }
static device_t devclass_get_device(int c, int unit) {
 assert(c == 1 && unit == 0); return available ? DEV : NULL;
}
static int device_is_attached(device_t d) { assert(d == DEV); return attached; }
static void sx_xlock(int *lock) { assert(lock == &sc.property_chan_lock && !*lock); *lock = 1; }
static void sx_xunlock(int *lock) { assert(lock == &sc.property_chan_lock && *lock); *lock = 0; }
static void device_printf(device_t d, const char *fmt, ...) { assert(d == DEV && fmt); }
static void *bcm2835_mbox_init_dma(device_t d, size_t len,
 bus_dma_tag_t *tag, bus_dmamap_t *map, bus_addr_t *phys) {
 assert(d == DEV && sc.property_chan_lock && len == sizeof(buffer)); allocations++;
 if (allocation_fails) return NULL;
 *tag = 11; *map = 12; *phys = address; return buffer;
}
static void bus_dmamap_sync(bus_dma_tag_t tag, bus_dmamap_t map, int flags) {
 assert(tag == 11 && map == 12 && sc.property_chan_lock && sync_count < 4);
 syncs[sync_count++] = flags;
}
static int fixture_write(device_t d, int chan, uint32_t value) {
 assert(d == DEV && chan == 8 && value == address); writes++; return write_error;
}
static int fixture_read(device_t d, int chan, uint32_t *value) {
 assert(d == DEV && chan == 8); reads++; *value = response_address;
 if (!read_error) memset(buffer, 0xa5, sizeof(buffer));
 return read_error;
}
#define MBOX_WRITE fixture_write
#define MBOX_READ fixture_read
static int bcm2835_mbox_err(device_t d, bus_addr_t phys, uint32_t reg,
 struct bcm2835_mbox_hdr *msg, size_t len) {
 assert(d == DEV && phys == address && msg && len == sizeof(buffer));
 validations++; return reg == address ? format_error : EIO;
}
static void bus_dmamap_unload(bus_dma_tag_t tag, bus_dmamap_t map) {
 assert(tag == 11 && map == 12); unloads++;
}
static void bus_dmamem_free(bus_dma_tag_t tag, void *buf, bus_dmamap_t map) {
 assert(tag == 11 && map == 12 && buf == buffer); frees++;
}
static void bus_dma_tag_destroy(bus_dma_tag_t tag) { assert(tag == 11); destroys++; }
static void fixture_lock(struct bcm_mbox_softc *s) { assert(s == &sc && !sc.lock); sc.lock = 1; }
#define MBOX_LOCK fixture_lock
static void fixture_unlock(struct bcm_mbox_softc *s) { assert(s == &sc && sc.lock); sc.lock = 0; }
#define MBOX_UNLOCK fixture_unlock
static uint32_t mbox_read_4(struct bcm_mbox_softc *s, int reg) {
 assert(s == &sc && sc.lock && reg == REG_STATUS); polls++;
 return empty_fifo ? STATUS_EMPTY : 0;
}
static void fixture_delay(unsigned us) { assert(us == 5 && sc.lock); delays++; }
#define DELAY fixture_delay
static int bcm_mbox_read_msg(struct bcm_mbox_softc *s, int *chan) {
 assert(s == &sc && sc.lock); fifo_reads++;
 *chan = foreign_channel ? 1 : 8; sc.msg[*chan] = address | (unsigned)*chan;
 return poll_error;
}
static int mtx_sleep(void *p, int *lock, int priority, const char *name, int ticks) {
 assert(p == &sc.have_message[8] && lock == &sc.lock && sc.lock);
 assert(priority == 0 && !strcmp(name,"mbox") && ticks == 10 * hz);
 sc.msg[8] = address | 8; sc.have_message[8] = 1; return 0;
}
'''

TEST = r'''
int main(int argc, char **argv) {
 assert(argc == 2);
 const char *name = argv[1];
 unsigned char msg[64] = {0}, original[64] = {0};
 if (!strcmp(name,"resource-refusal")) {
  disabled = 1; assert(bcm_mbox_acpi_probe(DEV) == ENXIO && !sc.acpi);
  disabled = 0; id_error = ENXIO; assert(bcm_mbox_acpi_probe(DEV) == ENXIO && !sc.acpi);
  id_error = 0; memory_start = 0xfe00b840; assert(bcm_mbox_acpi_probe(DEV) == ENXIO && !sc.acpi);
  memory_start = 0xfe00b880; memory_count = 0x23;
  assert(bcm_mbox_acpi_probe(DEV) == ENXIO && !sc.acpi && !descriptions);
  memory_count = 0x24; assert(!bcm_mbox_acpi_probe(DEV) && sc.acpi && descriptions == 1);
 } else if (!strcmp(name,"soc-refusal")) {
  assert(bcm283x_vcbus_init_acpi(0xfe00b840) == ENXIO && !booted_soc_memcfg);
  assert(!bcm283x_vcbus_init_acpi(0xfe00b880));
  assert(booted_soc_memcfg == &bcm283x_memory_configs[1]);
  assert(!bcm283x_vcbus_init_acpi(0xfe00b880));
  booted_soc_memcfg = &bcm283x_memory_configs[2];
  assert(bcm283x_vcbus_init_acpi(0xfe00b880) == EBUSY);
  assert(booted_soc_memcfg == &bcm283x_memory_configs[2]);
 } else if (!strcmp(name,"dma-address-refusal")) {
  struct bcm_mbox_dma_result out;
  bus_dma_segment_t segment = { .ds_addr = 0xc0001000 };
  bcm2835_mbox_dma_cb(&out, &segment, 1, EIO); assert(out.error == EIO);
  bcm2835_mbox_dma_cb(&out, &segment, 2, 0); assert(out.error == EFBIG);
  bcm2835_mbox_dma_cb(&out, &segment, 1, 0); assert(!out.error && out.address == 0xc0001000);
  segment.ds_addr++; bcm2835_mbox_dma_cb(&out, &segment, 1, 0); assert(out.error == EINVAL);
  segment.ds_addr = UINT64_C(0x100000000);
  bcm2835_mbox_dma_cb(&out, &segment, 1, 0); assert(out.error == EINVAL);
 } else if (!strcmp(name,"provider-absent")) {
  available = 0; assert(bcm2835_mbox_property(msg,sizeof(msg)) == ENXIO);
  available = 1; attached = 0; assert(bcm2835_mbox_property(msg,sizeof(msg)) == ENXIO);
  assert(!allocations && !sc.property_chan_lock);
 } else if (!strcmp(name,"allocation-failure")) {
  allocation_fails = 1; assert(bcm2835_mbox_property(msg,sizeof(msg)) == ENOMEM);
  assert(allocations == 1 && !writes && !frees && !sc.failed && !sc.property_chan_lock);
 } else if (!strcmp(name,"unsent-request")) {
  write_error = EAGAIN; assert(bcm2835_mbox_property(msg,sizeof(msg)) == EAGAIN);
  assert(writes == 1 && !reads && !validations && !sc.failed);
  assert(sync_count == 2 && syncs[0] == 3 && syncs[1] == 12);
  assert(frees == 1 && unloads == 1 && destroys == 1 && !memcmp(msg,original,sizeof(msg)));
 } else if (!strcmp(name,"late-dma-timeout") || !strcmp(name,"wrong-response")) {
  if (!strcmp(name,"late-dma-timeout")) read_error = ETIMEDOUT;
  else response_address += 16;
  int expected = read_error ? ETIMEDOUT : EIO;
  assert(bcm2835_mbox_property(msg,sizeof(msg)) == expected);
  assert(sc.failed && allocations == 1 && writes == 1 && reads == 1);
  assert(!frees && !unloads && !destroys && !validations && sync_count == 1);
  assert(syncs[0] == 3 && !memcmp(msg,original,sizeof(msg)) && !sc.property_chan_lock);
  assert(bcm2835_mbox_property(msg,sizeof(msg)) == EIO);
  assert(allocations == 1 && writes == 1 && reads == 1 && !frees && !sc.property_chan_lock);
 } else if (!strcmp(name,"completed-request") || !strcmp(name,"firmware-error")) {
  if (!strcmp(name,"firmware-error")) format_error = EIO;
  assert(bcm2835_mbox_property(msg,sizeof(msg)) == format_error);
  assert(allocations == 1 && writes == 1 && reads == 1 && validations == 1);
  assert(frees == 1 && unloads == 1 && destroys == 1 && !sc.failed && !sc.property_chan_lock);
  assert(sync_count == 2 && syncs[0] == 3 && syncs[1] == 12);
  for (unsigned i = 0; i < sizeof(msg); i++) assert(msg[i] == 0xa5);
 } else if (!strcmp(name,"cold-empty-timeout") || !strcmp(name,"poll-empty-timeout")) {
  cold = !strcmp(name,"cold-empty-timeout"); sc.polling = !cold; empty_fifo = 1;
  uint32_t value = 0x12345678;
  assert(bcm_mbox_read(DEV,8,&value) == ETIMEDOUT && !sc.lock);
  assert(polls == 200000 && delays == 199999 && !fifo_reads && value == 0x12345678);
 } else if (!strcmp(name,"foreign-channel-timeout")) {
  sc.polling = 1; foreign_channel = 1; uint32_t value = 0;
  assert(bcm_mbox_read(DEV,8,&value) == ETIMEDOUT && !sc.lock);
  assert(polls == 200000 && fifo_reads == 199999 && !delays);
 } else if (!strcmp(name,"poll-message") || !strcmp(name,"poll-message-error")) {
  sc.polling = 1; if (!strcmp(name,"poll-message-error")) poll_error = EIO;
  uint32_t value = 0;
  assert(bcm_mbox_read(DEV,8,&value) == (poll_error ? EINVAL : 0) && !sc.lock);
  assert(polls == 1 && fifo_reads == 1 && !delays);
  if (!poll_error) assert(value == address && !sc.msg[8] && !sc.have_message[8]);
 } else if (!strcmp(name,"irq-message")) {
  uint32_t value = 0;
  assert(!bcm_mbox_read(DEV,8,&value) && value == address && !sc.lock);
  assert(!sc.msg[8] && !sc.have_message[8] && !polls && !fifo_reads && !delays);
 } else return 64;
 printf("PASS: %s\n",name);
 return 0;
}
'''

CASES = (
    "resource-refusal", "soc-refusal", "dma-address-refusal", "provider-absent",
    "allocation-failure", "unsent-request", "late-dma-timeout", "wrong-response",
    "completed-request", "firmware-error", "cold-empty-timeout", "poll-empty-timeout",
    "foreign-channel-timeout", "poll-message", "poll-message-error", "irq-message",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    mbox = BASE / "bcm2835_mbox.c"
    vcbus = BASE / "bcm2835_vcbus.c"
    selected = [function(mbox, name) for name in (
        "bcm_mbox_acpi_probe", "bcm2835_mbox_dma_cb", "bcm2835_mbox_property", "bcm_mbox_read")]
    selected.append(function(vcbus, "bcm283x_vcbus_init_acpi"))
    (work / "control.c").write_text(PREFIX + "\n".join(selected) + TEST)
    source_hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (mbox, vcbus, BASE / "bcm2835_vcbus.h")}
    result = {"scope": "production functions with userspace service stubs; not hardware acceptance",
              "source_sha256": source_hashes, "cases": {}}

    def run(name, argv):
        with (work / (name + ".stdout")).open("xb") as stdout, \
             (work / (name + ".stderr")).open("xb") as stderr:
            rc = subprocess.run(argv, stdout=stdout, stderr=stderr, timeout=60).returncode
        result["cases"][name] = {"argv": argv, "exit": rc}
        (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        if rc:
            raise RuntimeError(f"{name} failed; retained output in {work}")

    flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
    binary = str(work / "control")
    run("compile", [args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                    str(work / "control.c"), "-o", binary])
    for case in CASES:
        run(case, [binary, case])
        print(f"PASS: {case}")
    assert all(hashlib.sha256((ROOT / p).read_bytes()).hexdigest() == value
               for p, value in source_hashes.items())
    result["production_sources_unchanged"] = True
    result["all_16_controls_passed"] = True
    (work / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
