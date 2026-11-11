#!/usr/bin/env python3
"""Compile the actual reset-provider translation unit against bounded services.

Tests exercise real callbacks, MMIO sequencing, publication and resource owners.
The fake registers do not qualify real reset/calibration hardware or PCIe.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile

ROOT=pathlib.Path(__file__).resolve().parents[3]
DRIVER=ROOT/'sys/arm64/broadcom/bcmstb_reset.c'
PREFIX=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
struct mtx { bool initialized,held; };
struct resource { uint64_t size;uint32_t words[32]; };
struct fake_device { void *softc; bool rescal,okay; int cellbytes;uint32_t cells; };
typedef struct fake_device *device_t;
typedef int phandle_t;
typedef uint32_t pcell_t;
typedef uint64_t rman_res_t;
typedef struct { const char *name; const void *method; } device_method_t;
typedef struct { const char *name;void *methods;size_t size; } driver_t;
#define DEVMETHOD(name,method) {#name,(const void *)&method}
#define DEVMETHOD_END {NULL,NULL}
#define EARLY_DRIVER_MODULE(name,bus,driver,...) static void *driver_ref __attribute__((used))=&driver
#define MODULE_VERSION(...)
#define SYS_RES_MEMORY 1
#define RF_ACTIVE 1
#define BUS_SPACE_BARRIER_WRITE 1
#define BUS_PASS_RESOURCE 1
#define BUS_PASS_ORDER_EARLY 1
#define BUS_PROBE_DEFAULT 0
#define MTX_DEF 0
static struct resource mem;
static unsigned reads,writes,barriers,delays,registrations,allocations,releases;
static unsigned alloc_error,release_error,sw_stuck,start_drop,clear_stuck;
static unsigned status_reads,done_after;
static bool mode_rescal;
static uint32_t last_offset,last_value;
static void *device_get_softc(device_t d) { return d->softc; }
static phandle_t ofw_bus_get_node(device_t d) { (void)d;return 1; }
static phandle_t OF_xref_from_node(phandle_t node) { return node+100; }
static int ofw_bus_status_okay(device_t d) { return d->okay; }
static int ofw_bus_is_compatible(device_t d,const char *c) {
 return strcmp(c,d->rescal?"brcm,bcm7216-pcie-sata-rescal":"brcm,brcmstb-reset")==0;
}
static device_t current;
static int OF_getproplen(phandle_t node,const char *name) {
 assert(node==1 && strcmp(name,"#reset-cells")==0);return current->cellbytes;
}
static int OF_getencprop(phandle_t node,const char *name,void *out,size_t size) {
 assert(node==1 && strcmp(name,"#reset-cells")==0 && size==4);
 if(current->cellbytes==4)memcpy(out,&current->cells,4);
 return current->cellbytes;
}
static void device_set_desc(device_t d,const char *s) { (void)d;(void)s; }
static void device_printf(device_t d,const char *s,...) { (void)d;(void)s; }
static struct resource *bus_alloc_resource_any(device_t d,int kind,int *rid,int flags) {
 assert(d==current && kind==SYS_RES_MEMORY && *rid==0 && flags==RF_ACTIVE);
 if(alloc_error)return NULL;
 allocations++;return &mem;
}
static uint64_t rman_get_size(struct resource *r) { assert(r==&mem);return r->size; }
static int bus_release_resource(device_t d,int kind,int rid,struct resource *r) {
 assert(d==current && kind==SYS_RES_MEMORY && rid==0 && r==&mem && allocations>releases);
 if(release_error)return EBUSY;
 releases++;return 0;
}
static void mtx_init(struct mtx *m,const char *name,void *type,int flags) {
 (void)name;assert(type==NULL && flags==MTX_DEF && !m->initialized);m->initialized=1;
}
static void mtx_lock(struct mtx *m) { assert(m->initialized && !m->held);m->held=1; }
static void mtx_unlock(struct mtx *m) { assert(m->initialized && m->held);m->held=0; }
static void mtx_destroy(struct mtx *m) { assert(m->initialized && !m->held);m->initialized=0; }
static uint32_t bus_read_4(struct resource *r,uint32_t off) {
 assert(r==&mem && off%4==0 && off+4<=r->size && off/4<32);reads++;
 if(mode_rescal && off==8 && ++status_reads>=done_after)r->words[2]|=1;
 return r->words[off/4];
}
static void bus_write_4(struct resource *r,uint32_t off,uint32_t value) {
 assert(r==&mem && off%4==0 && off+4<=r->size && off/4<32);
 writes++;last_offset=off;last_value=value;
 if(mode_rescal) {
  assert(off==0);
  if(value&1) { if(start_drop)value&=~1U;status_reads=0;r->words[2]&=~1U; }
  else if(clear_stuck)value|=1;
 } else if(!sw_stuck) {
  unsigned bank=off/24,reg=off%24;assert(reg==0 || reg==4);
  if(reg==0)r->words[(bank*24+8)/4]|=value;
  else r->words[(bank*24+8)/4]&=~value;
 }
 r->words[off/4]=value;
}
static void bus_barrier(struct resource *r,uint32_t off,uint32_t len,int flags) {
 assert(r==&mem && flags==BUS_SPACE_BARRIER_WRITE && off+len<=r->size);barriers++;
}
static void delay_us(unsigned us) { delays+=us; }
#define DELAY(us) delay_us(us)
static void hwreset_register_ofw_provider(device_t d);
'''
TEST=r'''
static struct bcmstb_reset_softc sc;
static struct fake_device dev;
static void hwreset_register_ofw_provider(device_t d) {
 assert(d==&dev && sc.published && sc.lock_initialized && sc.mem==&mem);
 assert(writes==0);registrations++;
}
static void reset(bool rescal) {
 memset(&sc,0,sizeof(sc));memset(&mem,0,sizeof(mem));memset(&dev,0,sizeof(dev));
 dev.softc=&sc;dev.rescal=rescal;dev.okay=1;dev.cellbytes=4;dev.cells=rescal?0:1;
 mode_rescal=rescal;mem.size=rescal?16:48;current=&dev;
 reads=writes=barriers=delays=registrations=allocations=releases=0;
 alloc_error=release_error=sw_stuck=start_drop=clear_stuck=0;
 status_reads=0;done_after=2;last_offset=last_value=0;
}
int main(void) {
 intptr_t id=-1;pcell_t cell;bool asserted;
 reset(false);assert(bcmstb_reset_probe(&dev)==0);
 assert(bcmstb_reset_attach(&dev)==0 && registrations==1 && writes==0);
 assert(sc.published && sc.nbanks==2 && bcmstb_reset_detach(&dev)==EBUSY && releases==0);
 cell=44;assert(bcmstb_reset_map(&dev,101,1,&cell,&id)==0 && id==44);
 assert(bcmstb_reset_map(&dev,100,1,&cell,&id)==EINVAL);
 assert(bcmstb_reset_map(&dev,101,0,NULL,&id)==EINVAL);
 cell=64;assert(bcmstb_reset_map(&dev,101,1,&cell,&id)==EINVAL);
 mem.words[8]=0xa5a50000;
 assert(bcmstb_reset_assert(&dev,44,true)==0);
 assert(last_offset==24 && last_value==0x1000 && mem.words[8]==0xa5a51000);
 assert(bcmstb_reset_is_asserted(&dev,44,&asserted)==0 && asserted);
 assert(bcmstb_reset_assert(&dev,44,false)==0 && delays==100);
 assert(last_offset==28 && last_value==0x1000 && mem.words[8]==0xa5a50000);
 assert(bcmstb_reset_is_asserted(&dev,44,&asserted)==0 && !asserted);
 assert(bcmstb_reset_assert(&dev,-1,true)==EINVAL && bcmstb_reset_assert(&dev,64,true)==EINVAL);
 assert(bcmstb_reset_is_asserted(&dev,0,NULL)==EINVAL);
 for(cell=0;cell<64;cell++) {
  assert(bcmstb_reset_assert(&dev,cell,true)==0);
  assert(last_offset==(cell/32)*24 && last_value==(1U<<(cell%32)));
  assert(bcmstb_reset_assert(&dev,cell,false)==0);
 }
 sw_stuck=1;delays=0;
 assert(bcmstb_reset_assert(&dev,44,true)==ETIMEDOUT && delays==1000);
 reset(true);assert(bcmstb_reset_attach(&dev)==0 && writes==0 && registrations==1);
 assert(bcmstb_reset_map(&dev,101,0,NULL,&id)==0 && id==0);
 cell=0;assert(bcmstb_reset_map(&dev,101,1,&cell,&id)==EINVAL);
 assert(bcmstb_reset_assert(&dev,1,false)==EINVAL);
 mem.words[0]=0xabc00000;mem.words[2]=0x12340000;
 assert(bcmstb_reset_assert(&dev,0,true)==0 && writes==0);
 assert(bcmstb_reset_is_asserted(&dev,0,&asserted)==EOPNOTSUPP && writes==0);
 assert(bcmstb_reset_assert(&dev,0,false)==0 && writes==2 && barriers==2 && delays==100);
 assert(mem.words[0]==0xabc00000 && mem.words[2]==0x12340001);
 reset(true);assert(bcmstb_reset_attach(&dev)==0);
 mem.words[2]=0x12340001;done_after=3;
 assert(bcmstb_reset_assert(&dev,0,false)==0 && writes==2 && status_reads==3 && delays==200);
 /* A fresh START invalidates stale DONE; completion must belong to this pulse. */
 mem.words[0]|=1;writes=0;
 assert(bcmstb_reset_assert(&dev,0,true)==EBUSY && bcmstb_reset_assert(&dev,0,false)==EBUSY && writes==0);
 reset(true);assert(bcmstb_reset_attach(&dev)==0);start_drop=1;
 assert(bcmstb_reset_assert(&dev,0,false)==EIO && writes==1 && status_reads==0);
 reset(true);assert(bcmstb_reset_attach(&dev)==0);clear_stuck=1;
 assert(bcmstb_reset_assert(&dev,0,false)==EIO && (mem.words[0]&1));
 reset(true);assert(bcmstb_reset_attach(&dev)==0);done_after=UINT_MAX;
 assert(bcmstb_reset_assert(&dev,0,false)==ETIMEDOUT && delays==1000 && writes==1 && (mem.words[0]&1));
 assert(bcmstb_reset_detach(&dev)==EBUSY && releases==0);
 status_reads=0;mem.words[2]|=1;writes=0;
 assert(bcmstb_reset_assert(&dev,0,false)==EBUSY && writes==0);
 /* A timed-out retained START is not silently retriggered or claimed idle. */
 reset(false);dev.okay=0;assert(bcmstb_reset_probe(&dev)==ENXIO);
 reset(false);dev.cellbytes=0;assert(bcmstb_reset_attach(&dev)==EINVAL && allocations==0 && registrations==0);
 reset(false);dev.cellbytes=8;assert(bcmstb_reset_attach(&dev)==EINVAL && allocations==0);
 reset(false);mem.size=((uint64_t)INT_MAX/32+1)*24;assert(bcmstb_reset_attach(&dev)==EINVAL && releases==1 && registrations==0);
 reset(true);dev.cells=1;assert(bcmstb_reset_attach(&dev)==EINVAL && allocations==0);
 reset(false);alloc_error=1;assert(bcmstb_reset_attach(&dev)==ENXIO && registrations==0);
 reset(false);mem.size=25;assert(bcmstb_reset_attach(&dev)==EINVAL && allocations==1 && releases==1 && registrations==0);
 reset(true);mem.size=8;assert(bcmstb_reset_attach(&dev)==EINVAL && releases==1 && registrations==0);
 reset(false);mem.size=25;release_error=1;
 assert(bcmstb_reset_attach(&dev)==0 && sc.mem==&mem && !sc.published && registrations==0);
 assert(bcmstb_reset_assert(&dev,0,true)==ENXIO && writes==0);
 assert(bcmstb_reset_detach(&dev)==EBUSY && sc.mem==&mem);
 release_error=0;assert(bcmstb_reset_detach(&dev)==0 && sc.mem==NULL && releases==1);
 puts("PASS: production SW_INIT/RESCAL provider: bank44+all64bits, W1S/W1C preservation, real status/timeouts, deassert-only/pair calibration, no attach writes, malformed geometry/cells, publication and retained resource-owner failures");
}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc',default='cc')
    ap.add_argument('--sanitize',action='store_true')
    args=ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='bcmstb-reset-') as tmp:
        root=pathlib.Path(tmp)
        for header in re.findall(r'#include [<"]([^>"]+)[>"]',DRIVER.read_text()):
            f=root/header;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('/* bounded kernel service substitute */\n')
        src=root/'control.c';binary=root/'control'
        src.write_text(PREFIX+'\n#include "'+str(DRIVER)+'"\n'+TEST)
        command=[args.cc,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root),str(src),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(command,check=True)
        subprocess.run([str(binary)],check=True)


if __name__=='__main__':
    main()
