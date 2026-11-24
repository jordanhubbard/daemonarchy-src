#!/usr/bin/env python3
"""Compile the production ACPI SD attach/read/cleanup callbacks.

Fault-injected bus/slot services check configuration and failure unwinding;
they do not replace native compilation or actual SD card I/O.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'sys/dev/sdhci/sdhci_acpi.c'


def function(name):
    source = SOURCE.read_text()
    match = re.search(r'(?m)^static (?:int|uint32_t)\n' + name + r'\(', source)
    start = match.start()
    pos = source.index('{', start) + 1
    depth = 1
    end = pos
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define __unused __attribute__((unused))
typedef int device_t;
typedef size_t bus_size_t;
typedef unsigned u_int;
#define BUS_SPACE_BARRIER_READ 1
#define BUS_SPACE_BARRIER_WRITE 2
#define SYS_RES_IRQ 1
#define SYS_RES_MEMORY 2
#define RF_ACTIVE 1
#define RF_SHAREABLE 4
#define DEVICE_PROP_UINT64 1
#define INTR_TYPE_MISC 1
#define INTR_MPSAFE 2
struct resource { int rid; };
struct sdhci_slot { unsigned quirks,max_clk,opt; };
struct sdhci_acpi_device { const char *hid; int uid; const char *desc; unsigned quirks; };
struct sdhci_acpi_softc {
 struct sdhci_slot slot;
 struct resource *mem_res,*irq_res;
 void *intrhand;
 const struct sdhci_acpi_device *acpi_dev;
 uint64_t caps_mask,firmware_caps;
 bool bcm_32bit;
 bool slot_initialized;
};
static struct sdhci_acpi_softc sc;
static struct sdhci_acpi_device selected;
static struct resource irq={1},mem={2};
static unsigned sdhci_quirk_clear,sdhci_quirk_set;
static unsigned fail_at,allocations,releases,cleanups,inits,starts,barriers;
static uint64_t property;
static int property_size;
static uint32_t rawcaps=0xffffffff,rawcaps2=0xffffffff,seen_caps,seen_caps2;
static unsigned seen_quirks;
static void *device_get_softc(device_t d) { assert(d==1);return &sc; }
static void bus_barrier(struct resource *r,int a,int b,int c) {assert(r==&mem&&a==0&&b==0xff&&c==3);barriers++;}
static uint32_t bus_read_4(struct resource *r,bus_size_t off) {assert(r==&mem);return off==SDHCI_CAPABILITIES?rawcaps:off==SDHCI_CAPABILITIES2?rawcaps2:0x12345678;}
static const struct sdhci_acpi_device *sdhci_acpi_find_device(device_t d) {assert(d==1);return &selected;}
static struct resource *bus_alloc_resource_any(device_t d,int type,int *rid,int flags) {assert(d==1&&*rid==0&&flags==(RF_ACTIVE|((type==SYS_RES_IRQ&&(!strcmp(selected.hid,"BCM2847")||!strcmp(selected.hid,"BRCME88C")))?RF_SHAREABLE:0)));allocations++;if(fail_at==allocations)return NULL;return type==SYS_RES_IRQ?&irq:&mem;}
static int device_printf(device_t d,const char *fmt,...) {(void)d;(void)fmt;return 0;}
static long device_get_property(device_t d,const char *name,void *out,size_t len,int type) {assert(d==1&&!strcmp(name,"sdhci-caps-mask")&&len==8&&type==DEVICE_PROP_UINT64);*(uint64_t *)out=property;return property_size;}
static int sdhci_acpi_detach(device_t);
static uint32_t sdhci_acpi_read_4(device_t,struct sdhci_slot *,bus_size_t);
#define SDHCI_READ_4(d,s,o) sdhci_acpi_read_4(d,s,o)
static int sdhci_init_slot(device_t d,struct sdhci_slot *s,int n) {assert(d==1&&s==&sc.slot&&n==0);inits++;seen_caps=sdhci_acpi_read_4(d,s,SDHCI_CAPABILITIES);seen_caps2=sdhci_acpi_read_4(d,s,SDHCI_CAPABILITIES2);seen_quirks=s->quirks;return fail_at==3?EIO:0;}
static void sdhci_acpi_intr(void *arg) {(void)arg;}
static int bus_setup_intr(device_t d,struct resource *r,int flags,void *filter,void (*fn)(void *),void *arg,void **handle) {assert(d==1&&r==&irq&&flags==3&&filter==NULL&&fn==sdhci_acpi_intr&&arg==&sc);if(fail_at==4)return EIO;*handle=&sc;return 0;}
static void sdhci_start_slot(struct sdhci_slot *s) {assert(s==&sc.slot);starts++;}
static void sdhci_cleanup_slot(struct sdhci_slot *s) {assert(s==&sc.slot&&sc.slot_initialized);cleanups++;}
static void bus_teardown_intr(device_t d,struct resource *r,void *h) {assert(d==1&&r==&irq&&h==&sc);}
static int rman_get_rid(struct resource *r) {return r->rid;}
static void bus_release_resource(device_t d,int type,int rid,struct resource *r) {assert(d==1&&type==rid&&r->rid==rid);releases++;}
'''
TEST = r'''
static void reset(const char *hid) {
 memset(&sc,0,sizeof(sc));selected=(struct sdhci_acpi_device){.hid=hid};
 fail_at=allocations=releases=cleanups=inits=starts=barriers=0;
 property=0;property_size=-1;sdhci_quirk_clear=sdhci_quirk_set=0;
}
int main(void) {
 reset("BRCME88C");
 assert(sdhci_acpi_attach(1)==0&&starts==1&&sc.slot_initialized);
 assert((seen_caps&(SDHCI_CAN_VDD_180|SDHCI_CAN_VDD_300|SDHCI_CAN_DO_DMA|SDHCI_CAN_DO_ADMA2))==0);
 assert((seen_caps2&(SDHCI_CAN_SDR50|SDHCI_CAN_SDR104|SDHCI_CAN_DDR50))==0);
 assert((seen_caps&SDHCI_CAN_VDD_330)!=0);
 assert((seen_quirks&SDHCI_QUIRK_BROKEN_DMA)!=0);
 assert(sdhci_acpi_read_4(1,&sc.slot,0x100)==0x12345678);
 sdhci_acpi_detach(1);assert(cleanups==1&&releases==2);
 reset("BRCME88C");property=1ULL<<10;property_size=8;
 sdhci_quirk_clear=~0U;sdhci_quirk_set=SDHCI_QUIRK_FORCE_DMA;
 assert(sdhci_acpi_attach(1)==0);
 assert((seen_caps&(1U<<10))==0);
 assert(!(seen_quirks&SDHCI_QUIRK_FORCE_DMA));
 assert(seen_quirks&SDHCI_QUIRK_BROKEN_DMA);
 sdhci_acpi_detach(1);
 reset("BRCME88C");property=~0ULL;property_size=4;
 assert(sdhci_acpi_attach(1)==0&&(seen_caps&SDHCI_CAN_VDD_330));sdhci_acpi_detach(1);
 reset("AMDI0040");sdhci_quirk_set=SDHCI_QUIRK_FORCE_DMA;
 assert(sdhci_acpi_attach(1)==0);
 assert(seen_caps==rawcaps&&seen_caps2==rawcaps2);
 assert(seen_quirks==SDHCI_QUIRK_FORCE_DMA);sdhci_acpi_detach(1);
 for(unsigned failure=1;failure<=4;failure++) {
  reset("BRCME88C");fail_at=failure;assert(sdhci_acpi_attach(1)!=0);
  assert(starts==0&&cleanups==(failure==4));
  assert(releases==(failure==1?0:failure==2?1:2));
 }
 puts("PASS: production BCM2711 ACPI SD attachment, caps mask, PIO override, unrelated-controller preservation and four failure paths");
 return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default='cc')
    args = p.parse_args()
    header = (ROOT / 'sys/dev/sdhci/sdhci.h').read_text()
    definitions = '\n'.join(re.findall(r'^#define\s+SDHCI_\w+[^\n]*', header, re.M))
    with tempfile.TemporaryDirectory(prefix='pi4-acpi-sd-') as temp:
        source = Path(temp) / 'control.c'
        source.write_text('#include <stdint.h>\n' + definitions + '\n' + PREFIX + '\n' +
                          '\n'.join(function(n) for n in ['sdhci_acpi_read_4', 'sdhci_acpi_attach', 'sdhci_acpi_detach']) + TEST)
        binary = Path(temp) / 'control'
        subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-sign-compare', '-fsanitize=address,undefined',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
