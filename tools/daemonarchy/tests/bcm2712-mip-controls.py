#!/usr/bin/env python3
"""Compile the full selected MIP0 provider against bounded parent/MMIO faults."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
DRIVER=ROOT/'sys/arm64/broadcom/bcm2712_mip.c'
PREFIX=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
struct resource { uint64_t start,size;uint32_t words[48]; };
struct mtx { bool initialized,held; };
struct device { void *softc;int node; };
typedef struct device *device_t;
typedef int phandle_t;
typedef uint32_t pcell_t;
struct intr_irqsrc { unsigned isrc_handlers; };
typedef struct { const char *name;void *method; } device_method_t;
typedef struct { const char *name;void *methods;size_t size; } driver_t;
#define DEVMETHOD(n,m) {#n,(void *)&m}
#define DEVMETHOD_END {NULL,NULL}
#define EARLY_DRIVER_MODULE(n,b,d,...) static void *driver_ref __attribute__((used))=&d
#define MODULE_VERSION(...)
#define SYS_RES_MEMORY 1
#define RF_ACTIVE 1
#define MTX_DEF 1
#define BUS_PROBE_DEFAULT 0
#define BUS_SPACE_BARRIER_WRITE 1
#define powerof2(v) (((v)&((v)-1))==0)
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
static struct resource mem;
static struct device mip,parent,child,other;
static pcell_t regs[8],ranges[5],offset,icells;
static int reglen,rangelen,offsetlen,msicellslen;
static pcell_t msicells;
static unsigned allocations,releases,writes,registrations,parent_allocations;
static int alloc_error,release_error,claim_error,parent_error_at,publish_error,drop_offset;
static bool parent_missing,claimed;
static struct intr_irqsrc parent_sources[64];
static void *device_get_softc(device_t d) { assert(d==&mip);return d->softc; }
static const char *device_get_nameunit(device_t d) { (void)d;return "mip-test"; }
static void device_set_desc(device_t d,const char *s) { (void)d;(void)s; }
static void device_printf(device_t d,const char *s,...) { (void)d;(void)s; }
static phandle_t ofw_bus_get_node(device_t d) { return d->node; }
static bool ofw_bus_status_okay(device_t d) { return d==&mip; }
static bool ofw_bus_is_compatible(device_t d,const char *s) {
 return (d==&mip && !strcmp(s,"brcm,bcm2712-mip")) ||
  (d==&parent && !strcmp(s,"arm,gic-400"));
}
static int OF_getproplen(int node,const char *name) {
 if(node==2){assert(!strcmp(name,"#interrupt-cells"));return 4;}
 assert(node==1);
 if(!strcmp(name,"reg"))return reglen;
 if(!strcmp(name,"msi-ranges"))return rangelen;
 if(!strcmp(name,"msi-controller"))return 0;
 if(!strcmp(name,"#msi-cells"))return msicellslen;
 assert(!strcmp(name,"brcm,msi-offset"));return offsetlen;
}
static int OF_getencprop(int node,const char *name,void *out,int len) {
 const void *data;int size;
 if(node==2){assert(!strcmp(name,"#interrupt-cells"));data=&icells;size=4;}
 else if(!strcmp(name,"reg")){data=regs;size=reglen;}
 else if(!strcmp(name,"msi-ranges")){data=ranges;size=rangelen;}
 else if(!strcmp(name,"#msi-cells")){data=&msicells;size=msicellslen;}
 else {assert(!strcmp(name,"brcm,msi-offset"));data=&offset;size=offsetlen;}
 assert(len>=size);if(size>0)memcpy(out,data,size);return size;
}
static bool OF_hasprop(int node,const char *name) { assert(node==1 && !strcmp(name,"msi-controller"));return true; }
static int OF_node_from_xref(uint32_t x) { return x==42?2:0; }
static device_t OF_device_from_xref(uint32_t x) { return x==42 && !parent_missing?&parent:NULL; }
static uint32_t OF_xref_from_node(int node) { assert(node==1);return 100; }
static struct resource *bus_alloc_resource_any(device_t d,int type,int *rid,int flags) {
 assert(d==&mip && type==SYS_RES_MEMORY && *rid==0 && flags==RF_ACTIVE);
 if(alloc_error)return NULL;
 assert(allocations++==0);return &mem;
}
static int bus_release_resource(device_t d,int type,int rid,struct resource *r) {
 assert(d==&mip && type==SYS_RES_MEMORY && rid==0 && r==&mem && allocations==1);
 if(release_error)return EBUSY;
 allocations--;releases++;return 0;
}
static uint64_t rman_get_start(struct resource *r){return r->start;}
static uint64_t rman_get_size(struct resource *r){return r->size;}
static void bus_write_4(struct resource *r,unsigned off,uint32_t v) {
 assert(r==&mem && claimed && parent_allocations==64 && off<192 && off%4==0);
 writes++;if((int)off!=drop_offset)r->words[off/4]=v;
}
static uint32_t bus_read_4(struct resource *r,unsigned off) { assert(r==&mem && off<192 && off%4==0);return r->words[off/4]; }
static void bus_barrier(struct resource *r,unsigned off,unsigned len,int flags) {assert(r==&mem && off+len<=192 && flags==BUS_SPACE_BARRIER_WRITE);}
static void mtx_init(struct mtx *m,const char *n,const char *t,int flags) { (void)n;(void)t;(void)flags;assert(!m->initialized);m->initialized=true; }
static void mtx_destroy(struct mtx *m) { assert(m->initialized && !m->held);m->initialized=false; }
static void mtx_lock(struct mtx *m) { assert(m->initialized && !m->held);m->held=true; }
static void mtx_unlock(struct mtx *m) { assert(m->held);m->held=false; }
static int GIC_RESERVE_MSI_RANGE_CHECKED(device_t d,unsigned start,unsigned count) {
 assert(d==&parent && start==160 && count==64 && !claimed);
 if(claim_error)return claim_error;
 claimed=true;return 0;
}
static int GIC_ALLOC_MSIX(device_t d,unsigned start,unsigned count,struct intr_irqsrc **out) {
 assert(d==&parent && start==160 && count==64 && claimed && parent_allocations<64);
 if((int)parent_allocations==parent_error_at)return EIO;
 *out=&parent_sources[parent_allocations++];return 0;
}
static int intr_msi_register(device_t,uint32_t);
'''
TEST=r'''
static struct bcm2712_mip_softc sc;
static int intr_msi_register(device_t d,uint32_t xref) {
 assert(d==&mip && xref==100 && claimed && parent_allocations==64 && writes==6);
 if(publish_error)return publish_error;
 device_t pic;struct intr_irqsrc *src;
 assert(sc.ready);
 /* Registration can synchronously expose us to the first real consumer. */
 assert(bcm2712_mip_alloc_msix(d,&child,&pic,&src)==0);
 assert(bcm2712_mip_release_msix(d,&child,src)==0);
 registrations++;return 0;
}
static void reset(void) {
 assert(!allocations);memset(&sc,0,sizeof(sc));mip.softc=&sc;mip.node=1;parent.node=2;
 memset(&mem,0,sizeof(mem));mem.start=MIP_CPU_BASE;mem.size=192;
 pcell_t r[]={0x10,0x130000,0,0xc0,0xff,0xfffff000,0,0x1000};memcpy(regs,r,sizeof(r));
 pcell_t a[]={42,0,128,1,64};memcpy(ranges,a,sizeof(a));
 offset=msicells=0;offsetlen=4;msicellslen=-1;reglen=32;rangelen=20;icells=3;
 releases=writes=registrations=parent_allocations=0;alloc_error=release_error=claim_error=publish_error=0;
 drop_offset=parent_error_at=-1;parent_missing=claimed=false;memset(parent_sources,0,sizeof(parent_sources));
}
static void discard_retained_test_device(void) {
 /* Test disposal only: production has no parent unreserve API. */
 sc.claimed=false;sc.ready=false;claimed=false;assert(bcm2712_mip_cleanup(&mip)==0);
}
int main(void) {
 struct intr_irqsrc *slots[64],*group[64],foreign={0};device_t pic=NULL;
 uint64_t addr;uint32_t data;
 reset();assert(bcm2712_mip_probe(&mip)==0);msicellslen=4;
 assert(bcm2712_mip_alloc(&mip,&child,1,1,&pic,slots)==ENXIO);
 assert(bcm2712_mip_attach(&mip)==0 && sc.ready && registrations==1 && writes==6 && parent_allocations==64);
 assert(mem.words[MIP_MASK_VPU_L/4]==UINT32_MAX && mem.words[MIP_MASK_VPU_H/4]==UINT32_MAX &&
 mem.words[MIP_CFG_L/4]==UINT32_MAX && mem.words[MIP_CFG_H/4]==UINT32_MAX &&
 mem.words[MIP_MASK_HOST_L/4]==0 && mem.words[MIP_MASK_HOST_H/4]==0);
 for(unsigned i=0;i<64;i++) {
  assert(bcm2712_mip_alloc_msix(&mip,&child,&pic,&slots[i])==0 && pic==&parent);
  assert(bcm2712_mip_map(&mip,&child,slots[i],&addr,&data)==0 && addr==MIP_BUS_TARGET && data==i);
 }
 assert(sc.allocated==UINT64_MAX && bcm2712_mip_alloc_msix(&mip,&child,&pic,group)==ENOSPC);
 assert(bcm2712_mip_map(&mip,&other,slots[63],&addr,&data)==EINVAL);
 assert(bcm2712_mip_map(&mip,&child,&foreign,&addr,&data)==EINVAL);
 slots[63]->isrc_handlers=1;assert(bcm2712_mip_release_msix(&mip,&child,slots[63])==EBUSY && sc.allocated==UINT64_MAX);
 slots[63]->isrc_handlers=0;
 for(unsigned i=0;i<64;i++)assert(bcm2712_mip_release_msix(&mip,&child,slots[i])==0);
 assert(sc.allocated==0 && bcm2712_mip_release_msix(&mip,&child,slots[0])==EINVAL);
 assert(bcm2712_mip_alloc(&mip,&child,64,64,&pic,group)==0 && sc.allocated==UINT64_MAX);
 assert(bcm2712_mip_release(&mip,&other,64,group)==EINVAL && sc.allocated==UINT64_MAX);
 assert(bcm2712_mip_release(&mip,&child,32,group)==EINVAL);
 assert(bcm2712_mip_release(&mip,&child,64,group)==0 && sc.allocated==0);
 for(int n=1;n<=32;n*=2) {
  assert(bcm2712_mip_alloc(&mip,&child,n,32,&pic,group)==0);
  assert(bcm2712_mip_map(&mip,&child,group[0],&addr,&data)==0 && data%32==0);
  assert(bcm2712_mip_release(&mip,&child,n,group)==0);
 }
 assert(bcm2712_mip_alloc(&mip,&child,0,1,&pic,group)==EINVAL);
 assert(bcm2712_mip_alloc(&mip,&child,3,4,&pic,group)==EINVAL);
 assert(bcm2712_mip_alloc(&mip,&child,4,2,&pic,group)==EINVAL);
 assert(bcm2712_mip_cleanup(&mip)==EBUSY && allocations==1);discard_retained_test_device();
 for(unsigned failure=0;failure<19;failure++) {
  reset();switch(failure) {
  case 0:reglen=16;break;case 1:regs[4]=0x10;break;
  case 2:ranges[2]=160;break;case 3:ranges[3]=4;break;
  case 4:offset=8;break;case 5:parent_missing=true;break;
  case 6:alloc_error=1;break;case 7:mem.size=128;break;
  case 8:claim_error=EBUSY;break;case 9:parent_error_at=32;break;
  case 10:drop_offset=MIP_CFG_H;break;case 11:publish_error=ENOMEM;break;
  case 12:rangelen=16;break;case 13:ranges[4]=65;break;case 14:regs[3]=0xc4;break;
  case 15:offsetlen=8;break;case 16:icells=2;break;
  case 17:msicellslen=4;msicells=1;break;case 18:msicellslen=8;break;
  }
  int error=bcm2712_mip_attach(&mip);
  if(failure<9 || failure>=12) { assert(error!=0 && !allocations && !registrations && !writes && !claimed); }
  else {
   assert(error==0 && allocations==1 && sc.claimed && !sc.ready && !registrations);
   assert(bcm2712_mip_alloc_msix(&mip,&child,&pic,group)==ENXIO);
   assert(bcm2712_mip_cleanup(&mip)==EBUSY);
   if(failure==9)assert(parent_allocations==32 && !writes);
   discard_retained_test_device();
  }
 }
 reset();claim_error=EOPNOTSUPP;release_error=EBUSY;
 assert(bcm2712_mip_attach(&mip)==0 && !sc.claimed && allocations==1 && !sc.ready);
 release_error=0;assert(bcm2712_mip_cleanup(&mip)==0);
 puts("PASS: complete production MIP0 provider: all64 slots, full-width bitmap, owner/group/alignment/map/release/busy faults, raw PCI target, geometry, checked parent failures, MMIO readback and retained ownership");
}
'''
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='cc');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
 with tempfile.TemporaryDirectory(prefix='bcm2712-mip-') as tmp:
  root=Path(tmp)
  for name in re.findall(r'#include [<"]([^>"]+)[>"]',DRIVER.read_text()):
   f=root/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('/* bounded service replacement */\n')
  c=root/'test.c';binary=root/'test';c.write_text(PREFIX+'\n#include "'+str(DRIVER)+'"\n'+TEST)
  cmd=[a.cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-I'+str(root),str(c),'-o',str(binary)]
  if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd,check=True);subprocess.run([str(binary)],check=True)
if __name__=='__main__':main()
