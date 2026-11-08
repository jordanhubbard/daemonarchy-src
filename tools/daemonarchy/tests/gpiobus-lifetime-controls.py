#!/usr/bin/env python3
"""Compile production GPIO bus functions/types with actual allocation owners.

Newbus deletion invokes the production child_deleted callback; ivars are real
heap allocations. These controls cannot establish native Newbus concurrency.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]


def block(text,pattern):
    m=re.search(pattern,text,re.M)
    if not m:raise ValueError('missing production block '+pattern)
    start=m.start();pos=text.index('{',m.start());depth=1;end=pos+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    if text[end:end+1]==';':end+=1
    return text[start:end]


PREFIX=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
struct mtx { bool initialized, locked; };
struct rman { int rm_type;const char *rm_descr;bool initialized; };
struct resource_list { int initialized; };
struct fake_device { void *softc,*ivars;struct fake_device *parent;bool attached,enabled;unsigned flags; };
typedef struct fake_device *device_t;
typedef unsigned driver_t;
#define M_DEVBUF 1
#define M_TEMP 2
#define M_NOWAIT 1
#define M_ZERO 2
#define M_WAITOK 0
#define RMAN_ARRAY 1
#define DEVICE_UNIT_ANY (-1)
#define GPIOMAXNAME 64
static size_t strlcpy(char *d,const char *s,size_t len) { size_t n=strlen(s);assert(n<len);memcpy(d,s,n+1);return n; }
static int GPIO_PIN_GETNAME(device_t d,uint32_t p,char *out) { (void)d;(void)p;strcpy(out,"default");return 0; }
#define GPIOBUS_IVAR(d) ((struct gpiobus_ivar *)(d)->ivars)
#define KASSERT(test,msg) assert(test)
#define GPIOBUS_DONTWAIT 2
#define GPIOBUS_ASSERT_UNLOCKED(sc) assert(!(sc)->sc_mtx.locked)
static void mtx_sleep(void *p,struct mtx *m,int priority,const char *s,int timeout) { (void)p;(void)m;(void)priority;(void)s;(void)timeout;abort(); }
#define GPIOBUS_LOCK(sc) mutex_lock(&(sc)->sc_mtx)
#define GPIOBUS_UNLOCK(sc) mutex_unlock(&(sc)->sc_mtx)
#define GPIOBUS_LOCK_INIT(sc) mutex_init(&(sc)->sc_mtx)
#define GPIOBUS_LOCK_DESTROY(sc) mutex_destroy(&(sc)->sc_mtx)
#define mtx_initialized(m) ((m)->initialized)
static unsigned owners,fail_alloc,pinmax_error,pinlist_error,child_busy,delete_error,rman_busy;
static unsigned rman_owners,lock_owners,child_live,provider_published,child_attaches;
static unsigned identifies,hints,rman_init_error,manage_error;
static struct fake_device parent,bus,child;
static void *kalloc(size_t size,int flags) {
 void *p;if(fail_alloc){fail_alloc--;return NULL;}
 p=(flags&M_ZERO)?calloc(1,size):malloc(size);assert(p);owners++;return p;
}
static void kfree(void *p) { if(p){assert(owners);owners--;free(p);} }
static void mutex_init(struct mtx *m) { assert(!m->initialized);m->initialized=1;lock_owners++; }
static void mutex_lock(struct mtx *m) { assert(m->initialized && !m->locked);m->locked=true; }
static void mutex_unlock(struct mtx *m) { assert(m->initialized && m->locked);m->locked=false; }
static void mutex_destroy(struct mtx *m) { assert(m->initialized && !m->locked && lock_owners);m->initialized=0;lock_owners--; }
#define malloc(size,type,flags) kalloc(size,flags)
#define mallocarray(n,size,type,flags) kalloc((n)*(size),flags)
#define free(p,type) kfree(p)
static void *device_get_softc(device_t d) { return d->softc; }
static void *device_get_ivars(device_t d) { assert(d==&child && child_live);return d->ivars; }
static void device_set_ivars(device_t d,void *p) { d->ivars=p; }
static device_t device_get_parent(device_t d) { return d->parent; }
static unsigned device_get_flags(device_t d) { return d->flags; }
static void device_set_flags(device_t d,unsigned flags) { d->flags=flags; }
static bool device_is_attached(device_t d) { return d->attached; }
static void device_disable(device_t d) { d->enabled=false; }
static const char *device_get_nameunit(device_t d) { (void)d;return "gpiobus-test"; }
static void device_printf(device_t d,const char *s,...) { (void)d;(void)s; }
static void panic(const char *s,...) { (void)s;abort(); }
static int rman_init(struct rman *r) { assert(!r->initialized);if(rman_init_error)return EIO;r->initialized=true;rman_owners++;return 0; }
static int rman_manage_region(struct rman *r,uint64_t lo,uint64_t hi) { assert(r->initialized && lo==0 && hi==UINT64_MAX);return manage_error?EIO:0; }
static int rman_fini(struct rman *r) { assert(r->initialized);if(rman_busy)return EBUSY;r->initialized=false;rman_owners--;return 0; }
static int GPIO_PIN_MAX(device_t d,int *max) { assert(d==&parent);if(pinmax_error)return EIO;*max=3;return 0; }
static int GPIO_GET_PIN_LIST(device_t d,uint32_t *pins) { assert(d==&parent);if(pinlist_error)return EIO;for(unsigned i=0;i<4;i++)pins[i]=i;return 0; }
static void resource_list_free(struct resource_list *r) { (void)r; }
static void resource_list_init(struct resource_list *r) { r->initialized=1; }
static void ofw_gpiobus_register_provider(device_t d) { assert(d==&parent);provider_published++; }
static void ofw_gpiobus_unregister_provider(device_t d) { assert(d==&parent);assert(provider_published);provider_published--; }
static int bus_generic_detach(device_t d) { assert(d==&bus || d==&parent);if(child_busy)return EBUSY;child.attached=false;return 0; }
static void bus_generic_new_pass(device_t d) { assert(d==&bus);if(child_live && child.enabled)child_attaches++; }
static void bus_generic_driver_added(device_t d,driver_t *r) { (void)r;bus_generic_new_pass(d); }
static int bus_generic_suspend(device_t d) { (void)d;return 0; }
static int bus_generic_resume(device_t d) { (void)d;return 0; }
static int bus_generic_shutdown(device_t d) { (void)d;return 0; }
static void bus_identify_children(device_t d) { assert(d==&bus);identifies++; }
static void bus_enumerate_hinted_children(device_t d) { assert(d==&bus);hints++; }
static void bus_attach_children(device_t d) { assert(d==&bus);if(child_live && child.enabled)child.attached=true; }
static device_t GPIO_GET_BUS(device_t d) { assert(d==&parent);return &bus; }
static int device_get_children(device_t d,device_t **out,int *n) {
 assert(d==&bus);*n=child_live?1:0;*out=*n?kalloc(sizeof(device_t),0):NULL;
 if(*n)(*out)[0]=&child;
 return 0;
}
static void gpiobus_child_deleted(device_t,device_t);
static device_t make_gpioc(device_t);
#define BUS_ADD_CHILD(dev,order,name,unit) make_gpioc(dev)
static int device_delete_child(device_t d,device_t c) {
 assert(d==&bus && c==&child && child_live);
 if(delete_error)return EBUSY;
 gpiobus_child_deleted(d,c);child_live=0;child.ivars=NULL;return 0;
}
static int device_delete_children(device_t d) {
 assert(d==&bus);return child_live?device_delete_child(d,&child):0;
}
static device_t device_add_child(device_t d,const char *name,int unit) {
 assert(d==&parent && strcmp(name,"gpiobus")==0 && unit==DEVICE_UNIT_ANY);return &bus;
}
'''
TEST=r'''
static struct gpiobus_softc sc;
static device_t make_gpioc(device_t d) {
 assert(d==&bus && !child_live);child_live=1;child.parent=&bus;child.enabled=true;
 child.ivars=malloc(sizeof(struct gpiobus_ivar),M_DEVBUF,M_ZERO);return &child;
}
static void reset(void) {
 assert(owners==0 && rman_owners==0 && lock_owners==0);
 memset(&sc,0,sizeof(sc));memset(&parent,0,sizeof(parent));memset(&bus,0,sizeof(bus));memset(&child,0,sizeof(child));
 bus.softc=&sc;bus.parent=&parent;bus.enabled=true;parent.enabled=true;
 fail_alloc=pinmax_error=pinlist_error=child_busy=delete_error=rman_busy=0;
 provider_published=child_attaches=identifies=hints=rman_init_error=manage_error=0;
}
int main(int argc,char **argv) {
 assert(argc==2);reset();
 if(strcmp(argv[1],"ivars")==0) {
  sc.sc_npins=4;sc.sc_dev=&parent;pinlist_error=1;
  assert(gpiobus_add_gpioc(&bus)==EIO);
  assert(owners==0 && !child_live);
 } else if(strcmp(argv[1],"busy-detach")==0) {
  assert(gpiobus_init_softc(&bus)==0);child_busy=1;
  assert(gpiobus_detach(&bus)==EBUSY);
  assert(sc.sc_mtx.initialized && sc.sc_pins && rman_owners==1 && lock_owners==1);
  child_busy=0;assert(gpiobus_detach(&bus)==0);
  assert(owners==0 && rman_owners==0 && lock_owners==0);
 } else if(strcmp(argv[1],"init-failure")==0) {
  for(unsigned failure=0;failure<4;failure++) {
   if(failure==0)pinmax_error=1;
   if(failure==1)fail_alloc=1;
   if(failure==2)rman_init_error=1;
   if(failure==3)manage_error=1;
   assert(gpiobus_init_softc(&bus)!=0);
   assert(owners==0 && rman_owners==0 && lock_owners==0);
   pinmax_error=fail_alloc=rman_init_error=manage_error=0;
  }
  manage_error=rman_busy=1;
  assert(gpiobus_attach(&bus)==0 && !gpiobus_ready(&bus));
  assert(owners==1 && rman_owners==1 && lock_owners==0);
  assert(gpiobus_acquire_pin(&bus,0)==ENXIO);
  rman_busy=0;assert(gpiobus_detach(&bus)==0 && owners==0 && rman_owners==0);
 } else if(strcmp(argv[1],"children")==0) {
  assert(gpiobus_init_softc(&bus)==0);assert(gpiobus_add_gpioc(&bus)==0);
  delete_error=1;assert(gpiobus_detach(&bus)==EBUSY);
  assert(child_live && owners==3 && lock_owners==1 && rman_owners==1);
  delete_error=0;assert(gpiobus_detach(&bus)==0);
  assert(!child_live && owners==0 && lock_owners==0 && rman_owners==0);
 } else if(strcmp(argv[1],"deferred")==0) {
  gpio_pin_t pin=NULL;
  assert(gpiobus_add_bus_deferred(&parent)==&bus && provider_published==0);
  assert(gpiobus_attach(&bus)==0 && gpiobus_ready(&bus));bus.attached=true;
  assert(identifies==0 && hints==0 && child_live && child.attached);
  assert(gpiobus_acquire_bus(&bus,&child,GPIOBUS_DONTWAIT)==0);
  assert(gpiobus_detach(&bus)==EBUSY && child_live && sc.sc_owner==&child && lock_owners==1);
  /* A legitimate lease release may proceed after quarantine. */
  sc.sc_owner=NULL;sc.sc_attach_error=0;
  assert(gpio_pin_get_by_bus_pinnum(&bus,2,&pin)==0 && pin && sc.sc_pins[2].mapped);
  assert(gpiobus_detach(&bus)==EBUSY && sc.sc_mtx.initialized && sc.sc_pins);
  assert(!gpiobus_ready(&bus) && !child_live);
  assert(gpiobus_acquire_pin(&bus,1)==ENXIO);
  gpio_pin_release(pin);
  assert(gpiobus_detach(&bus)==0 && owners==0 && lock_owners==0 && rman_owners==0);
 } else if(strcmp(argv[1],"attach-failure")==0) {
  assert(gpiobus_add_bus_deferred(&parent)==&bus);pinlist_error=1;delete_error=1;
  assert(gpiobus_attach(&bus)==0 && !gpiobus_ready(&bus));bus.attached=true;
  assert(child_live && child.ivars && !child.enabled && sc.sc_mtx.initialized && owners==3);
  gpiobus_new_pass(&bus);gpiobus_driver_added(&bus,NULL);
  assert(gpiobus_rescan(&bus)==ENXIO && child_attaches==0);
  assert(gpiobus_suspend(&bus)==ENXIO && gpiobus_resume(&bus)==ENXIO && gpiobus_shutdown(&bus)==ENXIO);
  assert(gpiobus_acquire_pin(&bus,1)==ENXIO);
  delete_error=0;assert(gpiobus_detach(&bus)==0 && owners==0 && rman_owners==0 && lock_owners==0);
 } else if(strcmp(argv[1],"raw-reservation")==0) {
  gpio_pin_t pin=NULL;char name[GPIOMAXNAME];
  assert(gpiobus_pin_getname(NULL,0,name)==ENXIO);
  assert(gpiobus_pin_setname(NULL,0,"test")==ENXIO);
  assert(gpiobus_acquire_pin(NULL,0)==ENXIO);
  assert(gpiobus_acquire_bus(NULL,&child,GPIOBUS_DONTWAIT)==ENXIO);
  assert(gpio_pin_get_by_child_index(NULL,0,&pin)==ENXIO);
  bus.softc=NULL;assert(gpiobus_acquire_pin(&bus,0)==ENXIO);bus.softc=&sc;
  assert(gpiobus_acquire_pin(&bus,0)==ENXIO);
  child.parent=&bus;assert(gpio_pin_get_by_child_index(&child,0,&pin)==ENXIO);
  assert(gpio_pin_get_by_bus_pinnum(&bus,0,&pin)!=0 && pin==NULL && owners==0);
  assert(gpiobus_init_softc(&bus)==0);
  assert(gpiobus_acquire_pin(&bus,4)==EINVAL);
  assert(gpiobus_pin_setname(&bus,4,"test")==EINVAL);
  assert(gpiobus_pin_getname(&bus,4,name)==EINVAL);
  assert(gpiobus_pin_setname(&bus,3,"test")==0);
  assert(gpiobus_pin_getname(&bus,3,name)==0 && !strcmp(name,"test"));
  gpiobus_quarantine(&bus,EIO);
  assert(gpiobus_acquire_pin(&bus,0)==ENXIO);
  assert(gpiobus_acquire_bus(&bus,&child,GPIOBUS_DONTWAIT)==ENXIO);
  assert(gpiobus_detach(&bus)==0 && owners==0 && rman_owners==0 && lock_owners==0);
 } else if(strcmp(argv[1],"rman-busy")==0) {
  assert(gpiobus_add_bus_deferred(&parent)==&bus && gpiobus_attach(&bus)==0);
  rman_busy=1;assert(gpiobus_detach(&bus)==EBUSY && sc.sc_pins && sc.sc_mtx.initialized);
  assert(!child_live && rman_owners==1 && owners==1);
  rman_busy=0;assert(gpiobus_detach(&bus)==0 && owners==0 && lock_owners==0 && rman_owners==0);
 } else if(strcmp(argv[1],"legacy")==0) {
  assert(gpiobus_add_bus(&parent)==&bus && provider_published==1);
  assert(gpiobus_attach(&bus)==0 && identifies==1 && hints==1);
  assert(gpiobus_detach(&bus)==0 && owners==0 && lock_owners==0 && rman_owners==0);
 } else abort();
 puts("PASS actual GPIO bus allocation, child callback and staged teardown owners");
}
'''


OFW_PREFIX=r'''
typedef int phandle_t;
static phandle_t ofw_bus_get_node(device_t dev) { assert(dev==&bus);return 1; }
static int OF_child(phandle_t node) { assert(node==1);return 0; }
static int OF_peer(phandle_t node) { (void)node;abort(); }
static int OF_hasprop(phandle_t node,const char *name) { (void)node;(void)name;abort(); }
static void *ofw_gpiobus_add_fdt_child(device_t dev,void *data,phandle_t node) { (void)dev;(void)data;(void)node;abort(); }
'''
OFW_TEST=r'''
static struct gpiobus_softc sc;
static device_t make_gpioc(device_t d) {
 assert(d==&bus && !child_live);child_live=1;child.parent=&bus;child.enabled=true;
 child.ivars=malloc(sizeof(struct gpiobus_ivar),M_DEVBUF,M_ZERO);return &child;
}
int main(void) {
 bus.softc=&sc;bus.parent=&parent;
 assert(gpiobus_add_bus_deferred(&parent)==&bus);
 pinlist_error=delete_error=1;
 assert(ofw_gpiobus_attach(&bus)==0 && !gpiobus_ready(&bus));
 assert(child_live && !child.enabled && owners==3 && lock_owners==1);
 assert(identifies==0 && hints==0);
 delete_error=pinlist_error=0;
 assert(gpiobus_detach(&bus)==0 && owners==0 && lock_owners==0);
 assert(ofw_gpiobus_attach(&bus)==0 && gpiobus_ready(&bus));
 assert(child_live && child.attached && identifies==0 && hints==0);
 assert(gpiobus_detach(&bus)==0 && owners==0);
 bus.flags=0;
 assert(ofw_gpiobus_attach(&bus)==0 && gpiobus_ready(&bus));
 assert(identifies==1 && hints==1);
 assert(gpiobus_detach(&bus)==0 && owners==0 && rman_owners==0 && lock_owners==0);
 puts("PASS: production OFW wrapper retains failed common owners, deferred mode omits hints/FDT enumeration, legacy normal path preserved");
}
'''

ACPI_PREFIX=r'''
typedef unsigned ACPI_STATUS, UINT32;
typedef uint64_t ACPI_PHYSICAL_ADDRESS, UINT64;
typedef void *ACPI_HANDLE;
typedef struct { void *Connection;unsigned Length; } ACPI_CONNECTION_INFO;
typedef struct { unsigned IoRestriction;unsigned *PinTable; } ACPI_RESOURCE_GPIO;
typedef struct { unsigned Type;struct { ACPI_RESOURCE_GPIO Gpio; } Data; } ACPI_RESOURCE;
#define ACPI_FAILURE(s) ((s)!=0)
#define AE_OK 0
#define AE_NOT_EXIST 1
#define AE_BAD_PARAMETER 2
#define ACPI_ADR_SPACE_GPIO 1
#define ACPI_RESOURCE_TYPE_GPIO 1
#define ACPI_WRITE 1
#define ACPI_READ 0
#define ACPI_IO_RESTRICT_INPUT 1
#define ACPI_IO_RESTRICT_OUTPUT 2
#define GPIO_PIN_HIGH 1
#define GPIO_PIN_LOW 0
#define ACPI_TYPE_DEVICE 1
#define ACPI_ROOT_OBJECT NULL
#define ACPI_UINT32_MAX UINT32_MAX
#define __predict_false(x) (x)
#define __containerof(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define ACPI_FREE(p) kfree(p)
static int handler_owners,install_fail,remove_fail,handle_missing,enumerations,aeis,buffer_reads;
static ACPI_HANDLE acpi_get_handle(device_t d) { assert(d==&parent);return handle_missing?NULL:&parent; }
static int AcpiBufferToResource(void *p,unsigned n,ACPI_RESOURCE **out) {
 (void)p;(void)n;(void)out;buffer_reads++;return AE_BAD_PARAMETER;
}
static int GPIO_PIN_SET(device_t d,unsigned p,unsigned v) { (void)d;(void)p;(void)v;abort(); }
static int GPIO_PIN_GET(device_t d,unsigned p,uint32_t *v) { (void)d;(void)p;(void)v;abort(); }
static int AcpiInstallAddressSpaceHandler(ACPI_HANDLE h,int type,
 ACPI_STATUS (*handler)(UINT32,ACPI_PHYSICAL_ADDRESS,UINT32,UINT64 *,void *,void *),void *setup,void *context) {
 assert(h==&parent && type==ACPI_ADR_SPACE_GPIO && handler && !setup && context);
 if(install_fail)return 1;
 handler_owners++;return AE_OK;
}
static int AcpiRemoveAddressSpaceHandler(ACPI_HANDLE h,int type,
 ACPI_STATUS (*handler)(UINT32,ACPI_PHYSICAL_ADDRESS,UINT32,UINT64 *,void *,void *)) {
 assert(h==&parent && type==ACPI_ADR_SPACE_GPIO && handler && handler_owners==1);
 if(remove_fail)return 1;
 handler_owners--;return AE_OK;
}
static void acpi_gpiobus_enumerate(void) {}
static int AcpiWalkNamespace(int t,void *root,unsigned max,void (*callback)(void),
 void *post,void *ctx,void *ret) {
 (void)t;(void)root;(void)max;(void)callback;(void)post;(void)ctx;(void)ret;enumerations++;return AE_OK;
}
'''
ACPI_TEST=r'''
static struct acpi_gpiobus_softc wrapper;
static device_t make_gpioc(device_t d) {
 assert(d==&bus && !child_live);child_live=1;child.parent=&bus;child.enabled=true;
 child.ivars=malloc(sizeof(struct gpiobus_ivar),M_DEVBUF,M_ZERO);return &child;
}
static void reset(void) {
 assert(owners==0 && rman_owners==0 && lock_owners==0 && handler_owners==0);
 memset(&wrapper,0,sizeof(wrapper));memset(&parent,0,sizeof(parent));memset(&bus,0,sizeof(bus));memset(&child,0,sizeof(child));
 bus.softc=&wrapper;bus.parent=&parent;bus.enabled=true;parent.enabled=true;
 fail_alloc=pinmax_error=pinlist_error=child_busy=delete_error=rman_busy=0;
 install_fail=remove_fail=handle_missing=enumerations=aeis=buffer_reads=0;
}
int main(void) {
 for(int failure=0;failure<3;failure++) {
  reset();delete_error=1;
  if(failure==0)pinlist_error=1;
  if(failure==1)handle_missing=1;
  if(failure==2)install_fail=1;
  assert(acpi_gpiobus_attach(&bus)==0 && !gpiobus_ready(&bus));
  assert(child_live && owners==3 && handler_owners==0 && !enumerations && !aeis);
  assert(acpi_gpiobus_space_handler(ACPI_READ,0,1,NULL,&wrapper.handler_info,NULL)==AE_NOT_EXIST && !buffer_reads);
  handle_missing=0;delete_error=0;
  assert(acpi_gpiobus_detach(&bus)==0 && owners==0 && lock_owners==0 && rman_owners==0);
 }
 reset();assert(acpi_gpiobus_attach(&bus)==0 && gpiobus_ready(&bus));
 assert(handler_owners==1 && wrapper.handler_installed && enumerations==1 && aeis==1);
 remove_fail=1;
 assert(acpi_gpiobus_detach(&bus)==EBUSY && handler_owners==1 && wrapper.handler_installed);
 assert(owners==3 && lock_owners==1 && rman_owners==1 && !gpiobus_ready(&bus));
 assert(acpi_gpiobus_space_handler(ACPI_READ,0,1,NULL,&wrapper.handler_info,NULL)==AE_NOT_EXIST && !buffer_reads);
 remove_fail=0;child_busy=1;
 assert(acpi_gpiobus_detach(&bus)==EBUSY && handler_owners==0 && !wrapper.handler_installed && owners==3);
 child_busy=0;assert(acpi_gpiobus_detach(&bus)==0 && owners==0 && lock_owners==0 && rman_owners==0);
 puts("PASS: production ACPI wrapper retains common/handler owners on faults, stops enumeration/raw handler access while quarantined and retries teardown once");
}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc',default='cc')
    p.add_argument('--case',choices=['ivars','busy-detach','init-failure','children','deferred','attach-failure','raw-reservation','rman-busy','legacy'])
    p.add_argument('--sanitize',action='store_true')
    a=p.parse_args()
    header=(ROOT/'sys/dev/gpio/gpiobusvar.h').read_text()
    source=(ROOT/'sys/dev/gpio/gpiobus.c').read_text()
    types='\n'.join(block(header,r'^struct '+name+r'\s*\n?\{') for name in ['gpiobus_pin_data','gpiobus_softc','gpiobus_pin','gpiobus_ivar'])
    names=['gpiobus_acquire_bus','gpiobus_pin_getname','gpiobus_pin_setname','gpiobus_add_bus','gpiobus_add_bus_deferred','gpiobus_acquire_pin','gpiobus_release_pin','gpio_pin_get_by_bus_pinnum','gpio_pin_get_by_child_index','gpio_pin_release','gpiobus_new_pass','gpiobus_driver_added','gpiobus_rescan','gpiobus_suspend','gpiobus_resume','gpiobus_shutdown','gpiobus_attach_failed','gpiobus_attach','gpiobus_ready','gpiobus_quarantine','gpiobus_alloc_ivars','gpiobus_free_ivars','gpiobus_child_deleted','gpiobus_init_softc','gpiobus_add_gpioc','gpiobus_detach']
    functions='\n'.join(block(source,r'^(?:static )?(?:int|void|bool|device_t)\s+'+name+r'\([^;]*?\n\{') for name in names)
    prototypes='\ntypedef struct gpiobus_pin *gpio_pin_t;\nbool gpiobus_ready(device_t);\nvoid gpiobus_quarantine(device_t,int);\nint gpiobus_acquire_pin(device_t,uint32_t);\nvoid gpiobus_release_pin(device_t,uint32_t);\nint gpiobus_init_softc(device_t);\nint gpiobus_add_gpioc(device_t);\nint gpiobus_detach(device_t);\nint gpiobus_alloc_ivars(struct gpiobus_ivar *);\nvoid gpiobus_free_ivars(struct gpiobus_ivar *);\n'
    with tempfile.TemporaryDirectory(prefix='gpiobus-lifetime-') as tmp:
        src=Path(tmp)/'control.c';binary=Path(tmp)/'control'
        constants=re.search(r'^#define GPIOBUS_FLAG_DEFER_PROVIDER .+$',header,re.M).group(0)+'\n'
        # Preserve production macro precedence and the void* Newbus API.
        constants+=re.search(r'^#define\s+GPIOBUS_SOFTC\(d\).+$',header,re.M).group(0)+'\n'
        acpi=(ROOT/'sys/dev/gpio/acpi_gpiobus.c').read_text()
        acpitypes='\n'.join(block(acpi,r'^struct '+name+r'\s*\n?\{') for name in ['acpi_gpiobus_softc','acpi_gpiobus_ctx'])
        acpifuncs='\n'.join(block(acpi,r'^static (?:int|ACPI_STATUS)\s+'+name+r'\([^;]*?\n\{') for name in ['acpi_gpiobus_space_handler','acpi_gpiobus_attach','acpi_gpiobus_detach'])
        acpistub='\nstatic void acpi_gpiobus_attach_aei(struct acpi_gpiobus_softc *sc,ACPI_HANDLE h) { assert(sc && h);aeis++; }\n'
        common='#define FDT 1\n'+PREFIX+constants+types+prototypes+functions
        ofw=(ROOT/'sys/dev/gpio/ofw_gpiobus.c').read_text()
        ofwfunc=block(ofw,r'^static int\s+ofw_gpiobus_attach\([^;]*?\n\{')
        programs={'core':common+TEST, 'ofw':common+OFW_PREFIX+ofwfunc+OFW_TEST, 'acpi':common+ACPI_PREFIX+acpitypes+acpistub+acpifuncs+ACPI_TEST}
        for program,contents in programs.items():
            src.write_text(contents)
            cmd=[a.cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-Wno-sign-compare',str(src),'-o',str(binary)]
            if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            subprocess.run(cmd,check=True)
            if program!='core':
                if not a.case:subprocess.run([str(binary)],check=True)
                continue
            for case in [a.case] if a.case else ['ivars','busy-detach','init-failure','children','deferred','attach-failure','raw-reservation','rman-busy','legacy']:
                subprocess.run([str(binary),case],check=True)


if __name__=='__main__':main()
