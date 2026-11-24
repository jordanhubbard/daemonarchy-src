#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise actual ACPI reservation policy with flattened namespace ownership.

The bounded rman fake rejects overlapping owners. This is a host control, not
a substitute for native kernel compilation and the Pi4 ACPI runtime fixture.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PATH = 'sys/dev/acpica/acpi.c'


def function(text, name):
    start = re.search(r'(?m)^static (?:bool|void)\n' + name + r'\(', text).start()
    pos = text.index('{', start)
    depth, end = 1, pos + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/queue.h>
typedef unsigned device_t;
typedef unsigned ACPI_HANDLE;
#define NULL_HANDLE 0U
#define NULL 0
#define ACPI_TYPE_DEVICE 1
#define ACPI_SUCCESS(v) ((v)==0)
#define SYS_RES_MEMORY 1
#define SYS_RES_IOPORT 2
#define SYS_RES_IRQ 3
#define M_TEMP 0
struct resource { unsigned owner,type;uint64_t start,end; };
struct resource_list_entry {
 STAILQ_ENTRY(resource_list_entry) link;
 unsigned type,rid;uint64_t start,end,count;struct resource *res;
};
STAILQ_HEAD(resource_list,resource_list_entry);
struct acpi_device { struct resource_list ad_rl; };
static struct {
 struct acpi_device ad;unsigned parent,namespace_parent;
 const char *hid;bool enabled,mapped;
 struct resource_list_entry entries[4];unsigned nentries;
} devices[8];
static struct resource pool[16];
static unsigned npool,reserve_calls,free_calls;
static bool children_fail,iterator_fail;
static char *sysres_ids[]={"PNP0C02",NULL};
static unsigned all_children[8],nchildren;
static ACPI_HANDLE acpi_get_handle(device_t dev) { assert(dev<8);return dev; }
static bool acpi_MatchHid(ACPI_HANDLE h,const char *id) { assert(h<8);return !strcmp(devices[h].hid,id); }
static int AcpiGetNextObject(int type,ACPI_HANDLE parent,ACPI_HANDLE prev,ACPI_HANDLE *next) {
 assert(type==ACPI_TYPE_DEVICE);if(iterator_fail)return 1;
 for(unsigned n=prev+1;n<8;n++)if(devices[n].namespace_parent==parent && devices[n].hid) {*next=n;return 0;}
 return 1;
}
static device_t acpi_get_device(ACPI_HANDLE h) { return devices[h].mapped?h:0; }
static device_t device_get_parent(device_t d) { return devices[d].parent; }
static bool device_is_enabled(device_t d) { return devices[d].enabled; }
static void *device_get_ivars(device_t d) { assert(d<8);return &devices[d].ad; }
static int device_get_children(device_t bus,device_t **out,int *count) {
 assert(bus==99);if(children_fail)return 1;*count=nchildren;
 *out=malloc(nchildren*sizeof(**out));assert(*out);
 memcpy(*out,all_children,nchildren*sizeof(**out));return 0;
}
static int ACPI_ID_PROBE(device_t bus,device_t dev,char **ids,void *out) {
 assert(bus==99 && ids==sysres_ids && !out);return !strcmp(devices[dev].hid,ids[0])?0:1;
}
static struct resource_list_entry *entry_find(struct resource_list *rl,unsigned type,unsigned rid) {
 struct resource_list_entry *r;STAILQ_FOREACH(r,rl,link)if(r->type==type && r->rid==rid)return r;abort();
}
static void resource_list_reserve(struct resource_list *rl,device_t bus,device_t owner,
 unsigned type,unsigned rid,uint64_t start,uint64_t end,uint64_t count,unsigned flags) {
 assert(bus==99 && !flags && count && end>=start);reserve_calls++;
 for(unsigned n=0;n<npool;n++)if(pool[n].type==type && start<=pool[n].end && pool[n].start<=end)return;
 assert(npool<16);pool[npool]=(struct resource){owner,type,start,end};
 entry_find(rl,type,rid)->res=&pool[npool++];
}
static void fake_free(void *p,int type) { assert(type==M_TEMP);free_calls++;free(p); }
#define free(p,t) fake_free(p,t)
static void reset(void) {
 memset(devices,0,sizeof(devices));memset(pool,0,sizeof(pool));
 npool=reserve_calls=free_calls=nchildren=0;children_fail=iterator_fail=false;
 for(unsigned n=0;n<8;n++) { STAILQ_INIT(&devices[n].ad.ad_rl);devices[n].hid="OTHER";
  devices[n].enabled=devices[n].mapped=true;devices[n].parent=99; }
 devices[1].hid="ACPI0004";devices[2].hid="BRCME88C";devices[2].namespace_parent=1;
 all_children[0]=1;all_children[1]=2;nchildren=2;
}
static struct resource_list_entry *add(unsigned dev,unsigned type,unsigned rid,uint64_t start,uint64_t len) {
 assert(devices[dev].nentries<4);struct resource_list_entry *r=&devices[dev].entries[devices[dev].nentries++];
 *r=(struct resource_list_entry){.type=type,.rid=rid,.start=start,.end=start+len-1,.count=len};
 STAILQ_INSERT_TAIL(&devices[dev].ad.ad_rl,r,link);return r;
}
static void fixture(void) {
 reset();add(1,SYS_RES_MEMORY,0,0xfe340000,0x100);add(2,SYS_RES_MEMORY,0,0xfe340000,0x100);
}
static bool late_driver_can_allocate(unsigned dev,unsigned rid) {
 struct resource_list_entry *r=entry_find(&devices[dev].ad.ad_rl,SYS_RES_MEMORY,rid);
 return r->res && r->res->owner==dev;
}
'''

BASELINE = r'''
int main(void) {
 fixture();acpi_reserve_resources(99);
 assert(npool==1 && pool[0].owner==1 && !late_driver_can_allocate(2,0));
 assert(free_calls==1);
 puts("PASS: original production policy reproduces GDV1 reservation blocking late BRCME88C allocation");
}
'''

TEST = r'''
int main(void) {
 for(unsigned order=0;order<2;order++) {
  fixture();if(order){all_children[0]=2;all_children[1]=1;}
  acpi_reserve_resources(99);
  assert(npool==1 && pool[0].owner==2 && !devices[1].entries[0].res);
  assert(late_driver_can_allocate(2,0) && free_calls==1);
 }
 /* Exact aliases only: distinct types, bounds, counts, namespace relationship,
  * disabled/missing children, different IDs and lookup errors keep ownership. */
 for(unsigned failure=0;failure<11;failure++) {
  fixture();
  switch(failure) {
  case 0:devices[1].hid="OTHER";break;
  case 1:devices[2].enabled=false;break;
  case 2:devices[2].mapped=false;break;
  case 3:devices[2].parent=98;nchildren=1;break;
  case 4:devices[2].namespace_parent=3;break;
  case 5:devices[2].entries[0].start++;devices[2].entries[0].count--;break;
  case 6:devices[2].entries[0].end--;devices[2].entries[0].count--;break;
  case 7:devices[2].entries[0].type=SYS_RES_IOPORT;break;
  case 8:iterator_fail=true;break;
  case 9:devices[2].entries[0].count--;break;
  case 10:devices[1].entries[0].count--;break;
  }
  acpi_reserve_resources(99);
  assert(devices[1].entries[0].res && devices[1].entries[0].res->owner==1);
 }
 fixture();devices[2].entries[0].rid=7;acpi_reserve_resources(99);
 assert(late_driver_can_allocate(2,7));
 fixture();add(1,SYS_RES_MEMORY,1,0xfe350000,0x100);acpi_reserve_resources(99);
 assert(npool==2 && devices[1].entries[1].res && late_driver_can_allocate(2,0));
 fixture();devices[1].entries[0].type=devices[2].entries[0].type=SYS_RES_IOPORT;
 acpi_reserve_resources(99);assert(npool==1 && pool[0].owner==2);
 fixture();devices[1].entries[0].type=devices[2].entries[0].type=SYS_RES_IRQ;
 acpi_reserve_resources(99);assert(npool==0 && reserve_calls==0);
 fixture();resource_list_reserve(&devices[1].ad.ad_rl,99,1,SYS_RES_MEMORY,0,0xfe340000,0xfe3400ff,0x100,0);
 acpi_reserve_resources(99);assert(npool==1 && pool[0].owner==1);
 fixture();resource_list_reserve(&devices[2].ad.ad_rl,99,2,SYS_RES_MEMORY,0,0xfe340000,0xfe3400ff,0x100,0);
 acpi_reserve_resources(99);assert(npool==1 && pool[0].owner==2);
 fixture();devices[3].hid="ACPI0004";devices[3].namespace_parent=1;
 devices[2].namespace_parent=3;add(3,SYS_RES_MEMORY,0,0xfe340000,0x100);
 all_children[1]=3;all_children[2]=2;nchildren=3;acpi_reserve_resources(99);
 assert(npool==1 && pool[0].owner==2 && !devices[1].entries[0].res && !devices[3].entries[0].res);
 fixture();children_fail=true;acpi_reserve_resources(99);assert(npool==0 && !free_calls);
 fixture();struct resource_list_entry invalid=devices[1].entries[0];invalid.count=0;
 assert(!acpi_syscont_child_resource(99,1,&invalid));invalid.count=256;invalid.end=0;
 assert(!acpi_syscont_child_resource(99,1,&invalid));
 puts("PASS: actual ACPI reservation policy gives exact container aliases to enabled immediate namespace children; unrelated/partial/active reservations and fault paths retained");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc', default='cc')
    p.add_argument('--baseline', action='store_true')
    args = p.parse_args()
    text = (ROOT / PATH).read_text()
    if args.baseline:
        text = subprocess.check_output(['git', 'show', '8b6683faa65adf855b5f9745266b577e1a24a316:' + PATH],
                                       cwd=ROOT, text=True)
    code = PREFIX.replace('#define NULL 0\n', '')
    if not args.baseline:
        # ACPI_HANDLE is an opaque pointer in the kernel; integer handles in
        # this finite model use the same zero sentinel without pointer casts.
        code += function(text, 'acpi_syscont_child_resource').replace('NULL', 'NULL_HANDLE')
    code += function(text, 'acpi_reserve_resources')
    code += BASELINE if args.baseline else TEST
    with tempfile.TemporaryDirectory(prefix='acpi-container-') as tmp:
        path = Path(tmp) / 'control.c'
        path.write_text(code)
        cmd = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
               str(path), '-o', str(path.with_suffix(''))]
        if 'clang' in Path(args.cc).name:
            probe = subprocess.run([args.cc, '-Werror', '-Wno-gcc-install-dir-libstdcxx',
                                    '-x', 'c', '-fsyntax-only', '-'], input='', text=True, capture_output=True)
            if probe.returncode == 0:
                cmd.insert(1, '-Wno-gcc-install-dir-libstdcxx')
        subprocess.run(cmd, check=True)
        subprocess.run([str(path.with_suffix(''))], check=True)
    print('source_sha256=' + hashlib.sha256(text.encode()).hexdigest())


if __name__ == '__main__':
    main()
