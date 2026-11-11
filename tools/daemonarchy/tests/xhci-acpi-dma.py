#!/usr/bin/env python3
"""Compile production xHCI ACPI _DMA parser/frontend with fault-injected services.

Bus-DMA/ACPICA substitutes exercise authored control flow; they do not establish
real bounce-buffer allocation or controller operation. Use an uncapped 8GB guest
for that acceptance gate.
"""
import argparse
import json
import struct
import pathlib
import re
import subprocess
import tempfile

ROOT=pathlib.Path(__file__).resolve().parents[3]


def function(path,name):
    text=(ROOT/path).read_text()
    start=re.search(r'(?m)^(?:static )?(?:int|void|ACPI_STATUS)\s+'+re.escape(name)+r'\(',text).start()
    pos=text.index('{',start)+1
    depth=1
    end=pos
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}')
        end+=1
    return text[start:end]


PREFIX=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
typedef int ACPI_STATUS;
typedef unsigned ACPI_HANDLE;
typedef unsigned device_t;
typedef unsigned driver_t;
typedef uint64_t bus_addr_t;
typedef void *bus_dma_tag_t;
#define AE_OK 0
#define AE_SUPPORT 1
#define AE_BAD_DATA 2
#define AE_NULL_ENTRY 3
#define AE_NOT_FOUND 4
#define AE_ERROR 5
#define ACPI_FAILURE(s) ((s)!=AE_OK)
#define ACPI_RESOURCE_TYPE_END_TAG 0
#define ACPI_RESOURCE_TYPE_ADDRESS16 1
#define ACPI_RESOURCE_TYPE_ADDRESS32 2
#define ACPI_RESOURCE_TYPE_ADDRESS64 3
#define ACPI_RESOURCE_TYPE_IRQ 4
#define ACPI_MEMORY_RANGE 0
#define ACPI_PRODUCER 0
#define ACPI_POS_DECODE 0
#define ACPI_ADDRESS_FIXED 1
#define PAGE_MASK 4095
#define BUS_SPACE_MAXADDR UINT64_MAX
#define BUS_SPACE_MAXSIZE UINT64_MAX
#define BUS_SPACE_UNRESTRICTED (~0)
/* Only field names read by the production parser are represented here.
 * The actual ACPICA declarations are checked by the native kernel build. */
typedef struct {
 unsigned ResourceType,ProducerConsumer,Decode,MinAddressFixed,MaxAddressFixed;
 struct { uint64_t Granularity,Minimum,Maximum,TranslationOffset,AddressLength; } Address;
 struct { unsigned StringLength; char *StringPtr; } ResourceSource;
} ACPI_RESOURCE_ADDRESS64;
typedef struct { unsigned Type; ACPI_RESOURCE_ADDRESS64 address; } ACPI_RESOURCE;
struct generic_xhci_acpi_dma { uint64_t lowaddr;unsigned ranges; };
#define XHCI_INIT_DMA 1
#define XHCI_INIT_CMD_LOCKS 2
#define SYS_RES_IRQ 1
#define SYS_RES_MEMORY 2
struct usb_bus { void *bdev; };
struct xhci_softc {
 bus_dma_tag_t sc_owned_dma_tag;
 void *sc_owned_dma_child;
 int sc_owned_dma_failed;
 uint8_t sc_owned_dma_started,sc_init_flags;
 void *sc_io_res,*sc_irq_res,*sc_intr_hdl;
 struct usb_bus sc_bus;int sc_cmd_cv,sc_cmd_sx;
};
static struct xhci_softc sc;
static ACPI_RESOURCE resources[3];
static unsigned nresources,method_owner=2,parent_error,walk_error,missing_crs;
static unsigned creates,destroys,attaches,create_error,attach_error;
static unsigned child_busy,halt_error,irq_error,destroy_error,release_error,usb_live;
static unsigned halts,unmaps,cv_destroys,sx_destroys,releases;
static unsigned child_disabled,later_passes,power_calls,driver_added_calls,rescan_calls;
static unsigned child_live,child_deleted,delete_error,ivars_live;
static void bus_generic_driver_added(device_t dev,driver_t *driver) { assert(dev==1);(void)driver;driver_added_calls++; }
static int bus_null_rescan(device_t dev) { assert(dev==1);rescan_calls++;return ENXIO; }
static int device_is_attached(void *dev) { assert(dev==(void *)2);return 0; }
static void device_disable(void *dev) { assert(dev==(void *)2);child_disabled=1; }
static void bus_generic_new_pass(device_t dev) { assert(dev==1);later_passes++; }
static int bus_generic_suspend(device_t dev) { assert(dev==1);power_calls++;return 0; }
static int bus_generic_resume(device_t dev) { assert(dev==1);power_calls++;return 0; }
static int bus_generic_shutdown(device_t dev) { assert(dev==1);power_calls++;return 0; }
static int generic_xhci_attach_failed(device_t,int);
int generic_xhci_detach(device_t);
void xhci_uninit(struct xhci_softc *);
static uint64_t created_lowaddr;
static ACPI_STATUS AcpiResourceToAddress64(ACPI_RESOURCE *res,ACPI_RESOURCE_ADDRESS64 *address) {
 *address=res->address;return AE_OK;
}
static ACPI_STATUS AcpiGetParent(ACPI_HANDLE child,ACPI_HANDLE *parent) {
 if(parent_error)return AE_ERROR;
 if(child>=4)return AE_NULL_ENTRY;
 *parent=child+1;return AE_OK;
}
static ACPI_STATUS AcpiGetHandle(ACPI_HANDLE parent,const char *name,ACPI_HANDLE *object) {
 if(strcmp(name,"_DMA")==0 && parent!=method_owner)return AE_NOT_FOUND;
 if(strcmp(name,"_CRS")==0 && missing_crs)return AE_NOT_FOUND;
 *object=parent;return AE_OK;
}
static ACPI_STATUS AcpiWalkResources(ACPI_HANDLE parent,const char *name,
    ACPI_STATUS (*cb)(ACPI_RESOURCE *,void *),void *arg) {
 unsigned i;ACPI_STATUS s;
 assert(parent==method_owner && strcmp(name,"_DMA")==0);
 if(walk_error)return AE_ERROR;
 for(i=0;i<nresources;i++) { s=cb(&resources[i],arg);if(s!=AE_OK)return s; }
 return AE_OK;
}
static ACPI_HANDLE acpi_get_handle(device_t dev) { assert(dev==1);return dev; }
static void *device_get_softc(device_t dev) { assert(dev==1);return &sc; }
static void device_printf(device_t dev,const char *fmt,...) { (void)dev;(void)fmt; }
static bus_dma_tag_t bus_get_dma_tag(device_t dev) { assert(dev==1);return (void *)7; }
static int bus_dma_tag_create(bus_dma_tag_t parent,unsigned align,unsigned boundary,
    uint64_t low,uint64_t high,void *filter,void *arg,uint64_t maxsize,int nseg,
    uint64_t maxseg,unsigned flags,void *lockfn,void *lockarg,bus_dma_tag_t *out) {
 assert(parent==(void *)7 && align==1 && boundary==0 && high==UINT64_MAX);
 assert(!filter && !arg && maxsize==UINT64_MAX && nseg==BUS_SPACE_UNRESTRICTED && maxseg==UINT64_MAX);
 assert(flags==0 && !lockfn && !lockarg);creates++;created_lowaddr=low;
 if(create_error)return ENOMEM;
 *out=(void *)8;return 0;
}
static int bus_generic_detach(device_t dev) {
 assert(dev==1);if(child_busy)return EBUSY;
 /* Real usb_bus_detach clears this field but leaves the Newbus child and
  * its ivars pointing into the parent softc until device_delete_child. */
 sc.sc_bus.bdev=NULL;return 0;
}
static int device_delete_child(device_t dev,void *child) {
 assert(dev==1 && child==(void *)2 && child_live && ivars_live);
 if(delete_error)return EBUSY;
 child_live=ivars_live=0;child_deleted++;return 0;
}
static int xhci_halt_controller(struct xhci_softc *arg) {
 assert(arg==&sc && sc.sc_io_res);halts++;return halt_error?1:0;
}
static int bus_teardown_intr(device_t dev,void *res,void *handle) {
 assert(dev==1 && res && handle);return irq_error?EIO:0;
}
static int rman_get_rid(void *res) { assert(res);return 0; }
static int bus_release_resource(device_t dev,int kind,int rid,void *res) {
 assert(dev==1 && (kind==SYS_RES_IRQ || kind==SYS_RES_MEMORY) && rid==0 && res);
 if(release_error==(unsigned)kind)return EBUSY;
 releases++;return 0;
}
static int bus_dma_tag_destroy(bus_dma_tag_t tag) {
 assert(tag==(void *)8 && usb_live==0 && !(sc.sc_init_flags & XHCI_INIT_DMA));
 if(destroy_error)return EBUSY;
 destroys++;return 0;
}
static int xhci_iterate_hw_softc;
static void usb_bus_mem_free_all(struct usb_bus *bus,int *callback) {
 assert(bus==&sc.sc_bus && callback==&xhci_iterate_hw_softc && usb_live==1);
 usb_live=0;unmaps++;
}
static void cv_destroy(int *cv) { assert(cv==&sc.sc_cmd_cv);cv_destroys++; }
static void sx_destroy(int *sx) { assert(sx==&sc.sc_cmd_sx);sx_destroys++; }
static int generic_xhci_attach(device_t dev) {
 assert(dev==1);attaches++;sc.sc_bus.bdev=(void *)2;
 child_live=ivars_live=1;
 if(sc.sc_owned_dma_tag)sc.sc_owned_dma_child=sc.sc_bus.bdev;
 sc.sc_io_res=(void *)9;sc.sc_irq_res=(void *)10;sc.sc_intr_hdl=(void *)11;
 /* Early failure precedes core DMA allocation and command-lock setup. */
 if(attach_error==1)return generic_xhci_attach_failed(dev,EIO);
 usb_live=1;sc.sc_init_flags=XHCI_INIT_DMA|XHCI_INIT_CMD_LOCKS;
 sc.sc_owned_dma_started=sc.sc_owned_dma_tag!=NULL;
 if(attach_error==2)return generic_xhci_attach_failed(dev,EIO);
 return 0;
}
'''
TEST=r'''
static void reset(void) {
 memset(&sc,0,sizeof(sc));memset(resources,0,sizeof(resources));
 nresources=1;method_owner=2;parent_error=walk_error=missing_crs=0;
 creates=destroys=attaches=create_error=attach_error=0;created_lowaddr=0;
 child_busy=halt_error=irq_error=destroy_error=release_error=usb_live=0;
 halts=unmaps=cv_destroys=sx_destroys=releases=0;
 child_disabled=later_passes=power_calls=driver_added_calls=rescan_calls=0;
 child_live=child_deleted=delete_error=ivars_live=0;
 resources[0].Type=ACPI_RESOURCE_TYPE_ADDRESS64;
 resources[0].address.ResourceType=FIXTURE_RESOURCE_TYPE;
 resources[0].address.ProducerConsumer=FIXTURE_GENERAL_FLAGS & 1;
 resources[0].address.Decode=(FIXTURE_GENERAL_FLAGS>>1) & 1;
 resources[0].address.MinAddressFixed=(FIXTURE_GENERAL_FLAGS>>2) & 1;
 resources[0].address.MaxAddressFixed=(FIXTURE_GENERAL_FLAGS>>3) & 1;
 resources[0].address.Address.Granularity=FIXTURE_GRANULARITY;
 resources[0].address.Address.Minimum=FIXTURE_MINIMUM;
 resources[0].address.Address.Maximum=FIXTURE_MAXIMUM;
 resources[0].address.Address.TranslationOffset=FIXTURE_TRANSLATION;
 resources[0].address.Address.AddressLength=FIXTURE_LENGTH;
}
int main(void) {
 bus_addr_t low;bool restricted;unsigned i;
 reset();assert(generic_xhci_acpi_dma_limit(1,&low,&restricted)==0);
 assert(restricted && low==0xbfffffff);
 assert(generic_xhci_acpi_attach(1)==0 && creates==1 && attaches==1);
 assert(created_lowaddr==0xbfffffff && sc.sc_owned_dma_tag==(void *)8);
 assert(generic_xhci_detach(1)==0 && unmaps==1 && destroys==1 && halts==1);
 assert(cv_destroys==1 && sx_destroys==1 && !sc.sc_owned_dma_tag);
 assert(generic_xhci_detach(1)==0 && unmaps==1 && cv_destroys==1);
 reset();method_owner=3;assert(generic_xhci_acpi_attach(1)==0 && created_lowaddr==0xbfffffff);
 reset();method_owner=0;assert(generic_xhci_acpi_attach(1)==0);
 assert(creates==0 && attaches==1 && sc.sc_owned_dma_tag==0);
 generic_xhci_new_pass(1);generic_xhci_driver_added(1,NULL);
 assert(later_passes==1 && driver_added_calls==1);
 assert(generic_xhci_rescan(1)==ENXIO && rescan_calls==1);
 assert(generic_xhci_suspend(1)==0 && generic_xhci_resume(1)==0 && generic_xhci_shutdown(1)==0 && power_calls==3);
 assert(generic_xhci_acpi_dma_limit(1,&low,&restricted)==0 && !restricted && low==UINT64_MAX);
 for(i=0;i<16;i++) {
  reset();
  switch(i) {
  case 0:resources[0].address.Address.TranslationOffset=0xc0000000;break;
  case 1:resources[0].address.Address.Minimum=4096;break;
  case 2:resources[0].address.Address.AddressLength=0;break;
  case 3:resources[0].address.Address.AddressLength--;break;
  case 4:resources[0].address.MinAddressFixed=0;break;
  case 5:resources[0].address.ResourceType=1;break;
  case 6:resources[1]=resources[0];nresources=2;break;
  case 7:nresources=0;break;
  case 8:resources[0].Type=ACPI_RESOURCE_TYPE_IRQ;break;
  case 9:resources[0].address.Address.Maximum=0x1000;resources[0].address.Address.AddressLength=0x1001;break;
  case 10:resources[0].address.Address.Maximum=0x7ff;resources[0].address.Address.AddressLength=0x800;break;
  case 11:resources[0].address.MaxAddressFixed=0;break;
  case 12:resources[0].address.ProducerConsumer=1;break;
  case 13:resources[0].address.Decode=1;break;
  case 14:resources[0].address.Address.Granularity=4095;break;
  case 15:resources[0].address.ResourceSource.StringLength=4;break;
  }
  assert(generic_xhci_acpi_attach(1)!=0 && creates==0 && attaches==0);
 }
 reset();missing_crs=1;assert(generic_xhci_acpi_attach(1)==EINVAL && creates==0);
 reset();walk_error=1;assert(generic_xhci_acpi_attach(1)==EOPNOTSUPP && creates==0);
 reset();parent_error=1;assert(generic_xhci_acpi_attach(1)==ENXIO && creates==0);
 reset();create_error=1;assert(generic_xhci_acpi_attach(1)==ENOMEM && creates==1 && attaches==0 && !sc.sc_owned_dma_tag);
 reset();attach_error=1;assert(generic_xhci_acpi_attach(1)==EIO && creates==1 && attaches==1 && destroys==1 && !sc.sc_owned_dma_tag);
 reset();attach_error=2;
 assert(generic_xhci_acpi_attach(1)==EIO && destroys==1 && unmaps==1 && halts==1);
 /* Newbus must retain the softc when cleanup cannot safely release owners. */
 reset();attach_error=2;halt_error=1;
 assert(generic_xhci_acpi_attach(1)==0 && sc.sc_owned_dma_failed==EIO);
 assert(sc.sc_owned_dma_tag && usb_live && destroys==0 && sc.sc_intr_hdl && sc.sc_io_res);
 assert(child_disabled);
 generic_xhci_new_pass(1);
 assert(later_passes==0);
 assert(generic_xhci_suspend(1)==ENXIO && generic_xhci_resume(1)==ENXIO && generic_xhci_shutdown(1)==ENXIO && power_calls==0);
 halt_error=0;assert(generic_xhci_detach(1)==0 && unmaps==1 && destroys==1);
 reset();attach_error=2;child_busy=1;
 assert(generic_xhci_acpi_attach(1)==0 && sc.sc_owned_dma_failed==EIO && halts==0 && destroys==0);
 child_busy=0;assert(generic_xhci_detach(1)==0 && unmaps==1 && destroys==1);
 reset();attach_error=2;delete_error=1;
 assert(generic_xhci_acpi_attach(1)==0 && sc.sc_owned_dma_failed==EIO);
 assert(sc.sc_bus.bdev==NULL && sc.sc_owned_dma_child==(void *)2 && child_disabled);
 assert(child_live && ivars_live && usb_live && destroys==0 && child_deleted==0);
 generic_xhci_new_pass(1);generic_xhci_driver_added(1,NULL);
 assert(later_passes==0 && driver_added_calls==0);
 delete_error=0;assert(generic_xhci_detach(1)==0);
 assert(!child_live && !ivars_live && child_deleted==1 && destroys==1 && unmaps==1);
 reset();attach_error=2;irq_error=1;
 assert(generic_xhci_acpi_attach(1)==0 && sc.sc_owned_dma_failed==EIO && destroys==0 && usb_live && sc.sc_intr_hdl);
 irq_error=0;assert(generic_xhci_detach(1)==0 && unmaps==1 && destroys==1 && halts==1);
 for(i=SYS_RES_IRQ;i<=SYS_RES_MEMORY;i++) {
  reset();attach_error=2;release_error=i;
  assert(generic_xhci_acpi_attach(1)==0 && sc.sc_owned_dma_failed==EIO);
  assert(sc.sc_owned_dma_tag && usb_live && destroys==0);
  if(i==SYS_RES_IRQ)assert(sc.sc_irq_res);
  else assert(sc.sc_io_res);
  release_error=0;assert(generic_xhci_detach(1)==0 && unmaps==1 && destroys==1);
 }
 reset();assert(generic_xhci_acpi_attach(1)==0);destroy_error=1;
 assert(generic_xhci_detach(1)==EBUSY && sc.sc_owned_dma_tag && unmaps==1 && cv_destroys==1);
 assert(sc.sc_owned_dma_failed==EBUSY && child_deleted==1 && !child_live && !ivars_live && !sc.sc_owned_dma_child);
 generic_xhci_new_pass(1);generic_xhci_driver_added(1,NULL);
 assert(generic_xhci_rescan(1)==ENXIO && generic_xhci_resume(1)==ENXIO);
 assert(later_passes==0 && driver_added_calls==0 && rescan_calls==0 && power_calls==0);
 destroy_error=0;assert(generic_xhci_detach(1)==0 && destroys==1 && unmaps==1 && cv_destroys==1);
 reset();xhci_uninit(&sc);assert(unmaps==0 && cv_destroys==0 && sx_destroys==0);
 puts("PASS: production ACPI _DMA parser/ancestor/frontend: exact3GiB limit, ancestor/no_DMA fallback, unsupported/malformed rejection, allocation/attach faults, halt/child/IRQ quarantine, retry without double-uninit, DMA-before-parent teardown");
}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc',default='cc')
    args=ap.parse_args()
    fixture=json.loads((ROOT/'tools/daemonarchy/tests/fixtures/pi4-xhci-dma-qword.json').read_text())
    raw=bytes.fromhex(fixture['qword_descriptor_hex'])
    assert len(raw)==46 and raw[:3]==bytes.fromhex('8a2b00')
    fields=dict(zip(['GRANULARITY','MINIMUM','MAXIMUM','TRANSLATION','LENGTH'],
                    struct.unpack_from('<5Q',raw,6)))
    fields.update(RESOURCE_TYPE=raw[3],GENERAL_FLAGS=raw[4])
    constants=''.join(f'#define FIXTURE_{key} 0x{value:x}ULL\n' for key,value in fields.items())
    path='sys/dev/usb/controller/generic_xhci_acpi.c'
    source=constants+PREFIX+function('sys/dev/usb/controller/xhci.c','xhci_uninit')+\
        function('sys/dev/usb/controller/generic_xhci.c','generic_xhci_quarantine')+\
        function('sys/dev/usb/controller/generic_xhci.c','generic_xhci_detach')+\
        function('sys/dev/usb/controller/generic_xhci.c','generic_xhci_attach_failed')+\
        ''.join(function('sys/dev/usb/controller/generic_xhci.c',n) for n in [
            'generic_xhci_new_pass','generic_xhci_driver_added','generic_xhci_rescan','generic_xhci_suspend','generic_xhci_resume',
            'generic_xhci_shutdown'])+\
        ''.join(function(path,n) for n in [
        'generic_xhci_acpi_dma_resource','generic_xhci_acpi_dma_limit',
        'generic_xhci_acpi_attach'])+TEST
    with tempfile.TemporaryDirectory(prefix='xhci-acpi-dma-') as tmp:
        src=pathlib.Path(tmp)/'control.c';binary=pathlib.Path(tmp)/'control'
        src.write_text(source)
        subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__=='__main__':
    main()
