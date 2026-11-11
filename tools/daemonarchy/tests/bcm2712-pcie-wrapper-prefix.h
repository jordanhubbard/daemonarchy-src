/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <arm64/broadcom/bcm2712_pcie_hw.h>
#define __unused __attribute__((unused))
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define STAILQ_INIT(a) (*(a)=0)
typedef unsigned u_int;
typedef uint64_t rman_res_t;
typedef int phandle_t;
typedef uint32_t cell_t;
typedef void driver_filter_t;
typedef void driver_intr_t;
struct mtx { bool initialized,held; };
struct rman { int rm_type;const char *rm_descr;bool initialized;uint64_t first,last; };
struct resource { uint64_t start,size;bool allocated;int type;unsigned flags;uint32_t words[0x9310/4]; };
struct device { void *softc;int node;const char *name;bool attached,disabled; };
typedef struct device *device_t;
struct tag { bool alive;unsigned children; };
typedef struct tag *bus_dma_tag_t;
typedef int *hwreset_t;
struct pcie_range { uint64_t pci_base,phys_base,size,flags;struct resource *res;int rid; };
struct generic_pcie_core_softc { struct pcie_range ranges[16];int nranges,coherent;bool has_pmem;struct rman pmem_rman,mem_rman,io_rman;struct resource *res;int bus_start,bus_end,ecam;device_t dev;uintptr_t ioh;bus_dma_tag_t dmat;uint32_t quirks; };
struct generic_pcie_fdt_softc { struct generic_pcie_core_softc base;int pci_iinfo,pci_ofw_devlist; };
typedef struct { const char *name;void *method; } device_method_t;
typedef struct { int dummy; } driver_t;
#define DEVMETHOD(n,m) {#n,(void *)&m}
#define DEVMETHOD_END {NULL,NULL}
#define DEFINE_CLASS_1(n,d,m,s,b) static driver_t d;static void *method_ref __attribute__((used))=m
#define EARLY_DRIVER_MODULE(n,b,d,...) static void *driver_ref __attribute__((used))=&d
#define MODULE_DEPEND(...)
#define SYS_RES_MEMORY 1
#define SYS_RES_IOPORT 2
#define RF_ACTIVE 1
#define RF_UNMAPPED 2
#define RF_PREFETCHABLE 4
#define RMAN_ARRAY 1
#define FLAG_TYPE_MEM 2
#define MAX_RANGES_TUPLES 16
#define FLAG_TYPE(x) ((x)&3)
#define FLAG_TYPE_IO 1
#define PCI_RES_BUS 3
#define FLAG_TYPE_PMEM 3
#define BUS_SPACE_MAXADDR UINT64_MAX
#define MTX_DEF 1
#define PCIE_CUSTOM_CONFIG_SPACE_QUIRK 2
#define BUS_SPACE_BARRIER_WRITE 1
#define DEVICE_UNIT_ANY -1
#define BUS_PROBE_DEFAULT 0
static struct device host,msi,gpio,pci;
static struct resource resources[3];
static struct tag physical_tag,translated_tag;
static int bridge_token,rescal_token;
static unsigned step,fail_step,writes,reads,index_writes,data_accesses,children,delegates,of_scans;
static int domain_error,resource_error,destroy_error,fini_error,release_error,msi_error,child_error,delete_error;
static bool entries[2],unknown_property,property_loop,child_missing,child_unattached;
static uint64_t mapped_address;static uint32_t mapped_data;
static unsigned msi_allocations,msi_releases,cleanup_calls,cleanup_fail_at;
static bool fault(void) { return ++step==fail_step; }
static void *device_get_softc(device_t d) { return d->softc; }
static bool device_is_attached(device_t d) { return d->attached; }
static const char *device_get_name(device_t d) { return d->name; }
static void device_set_desc(device_t d,const char *s) {(void)d;(void)s;}
static void device_printf(device_t d,const char *s,...) {(void)d;(void)s;}
static int ofw_bus_get_node(device_t d) {return d->node;}
#include "provider-fixture.h"
static bool provider_fixture;
static int missing_provider;
static const struct prop *prop(int n,const char *name) {for(unsigned i=0;i<nitems(props);i++)if(props[i].node==n&&!strcmp(props[i].name,name))return &props[i];return NULL;}
static uint32_t beword(const unsigned char *p) {return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static int OF_getproplen(int n,const char *name) {const struct prop *p=prop(n,name);return p?(int)p->size:-1;}
static int OF_getprop(int n,const char *name,void *out,unsigned len) {const struct prop *p=prop(n,name);if(!p)return -1;if(len>p->size)len=p->size;memcpy(out,p->data,len);return len;}
static int OF_getencprop(int n,const char *name,void *out,unsigned len) {const struct prop *p=prop(n,name);if(!p||p->size%4)return -1;if(len>p->size)len=p->size;for(unsigned i=0;i<len/4;i++)((uint32_t *)out)[i]=beword(p->data+4*i);return len;}
static int OF_nextprop(int n,const char *previous,char *next,size_t len) {
 assert(len>=128);of_scans++;
 /* Actual ofw_fdt.c: NULL starts; empty string is not a first-property alias. */
 if(!provider_fixture){
  if(previous==NULL){strcpy(next,"compatible");return 1;}
  if(property_loop){strcpy(next,previous);return 1;}
  if(!strcmp(previous,"compatible")&&unknown_property){strcpy(next,"unsupported-property");return 1;}
  return 0;
 }
 bool found=previous==NULL;
 for(unsigned i=0;i<nitems(props);i++)if(props[i].node==n){if(found){strcpy(next,props[i].name);return 1;}if(!strcmp(previous,props[i].name))found=true;}
 return 0;
}
static int OF_node_from_xref(int x) {for(unsigned i=0;i<nitems(props);i++)if(!strcmp(props[i].name,"phandle")&&beword(props[i].data)==(uint32_t)x)return props[i].node;return 0;}
static int OF_xref_from_node(int n) {const struct prop *p=prop(n,"phandle");return p?(int)beword(p->data):n;}
static int OF_parent(int n) {return n>0&&(unsigned)n<nitems(parents)?parents[n]:0;}
static int OF_child(int n) {for(unsigned i=1;i<nitems(parents);i++)if(parents[i]==n)return i;return 0;}
static int OF_peer(int n) {for(unsigned i=n+1;i<nitems(parents);i++)if(parents[i]==OF_parent(n))return i;return 0;}
static bool OF_hasprop(int n,const char *s) {return prop(n,s)!=NULL;}
static struct device reset_provider[2];
static device_t OF_device_from_xref(int n) {
 if(n==missing_provider)return NULL;
 if(n==0x27)return &msi;
 if(n==OF_xref_from_node(gpio.node))return &gpio;
 if(n==0x23||n==0x24)return &reset_provider[n-0x23];
 return NULL;
}
static bool ofw_bus_node_is_compatible(int n,const char *s) {const struct prop *p=prop(n,"compatible");if(!p)return false;for(unsigned i=0;i<p->size;){const char *v=(const char *)p->data+i;if(!strcmp(v,s))return true;i+=strlen(v)+1;}return false;}
static bool ofw_bus_node_status_okay(int n) {const struct prop *p=prop(n,"status");return !p||!strcmp((const char *)p->data,"okay");}
static bool ofw_bus_status_okay(device_t d) {return ofw_bus_node_status_okay(d->node);}
static bool ofw_bus_is_compatible(device_t d,const char *s) {return ofw_bus_node_is_compatible(d->node,s);}
static int intr_msi_validate_provider(device_t d,intptr_t x) {return d==&msi&&x==0x27?msi_error:ENXIO;}
static int hwreset_get_by_ofw_name(device_t d,int n,const char *name,hwreset_t *out) {(void)d;(void)n;if(fault())return EIO;*out=!strcmp(name,"bridge")?&bridge_token:&rescal_token;return 0;}
static void hwreset_release(hwreset_t t) {assert(t==&bridge_token||t==&rescal_token);}
static int hwreset_assert(hwreset_t t) {(void)t;writes++;return 0;}
static int hwreset_deassert(hwreset_t t) {(void)t;writes++;return 0;}
static struct resource *bus_alloc_resource_any(device_t d,int type,int *rid,unsigned flags) {(void)d;(void)type;(void)flags;if(fault())return NULL;unsigned i=*rid==0?0:*rid-99;assert(i<3&&!resources[i].allocated);resources[i].allocated=true;return &resources[i];}
static uint64_t rman_get_start(struct resource *r) {return r->start;}
static uint64_t rman_get_size(struct resource *r) {return r->size;}
static int bus_release_resource(device_t d,int type,int rid,struct resource *r) {(void)d;(void)type;(void)rid;if(++cleanup_calls==cleanup_fail_at)return EBUSY;if(release_error)return release_error;assert(r->allocated);r->allocated=false;return 0;}
static void mtx_init(struct mtx *m,const char *s,void *p,int f) {(void)s;(void)p;(void)f;assert(!m->initialized);m->initialized=true;}
static void mtx_lock(struct mtx *m) {assert(m->initialized&&!m->held);m->held=true;}
static void mtx_unlock(struct mtx *m) {assert(m->held);m->held=false;}
static void mtx_destroy(struct mtx *m) {assert(m->initialized&&!m->held);m->initialized=false;}
static bus_dma_tag_t bus_get_dma_tag(device_t d) {(void)d;return NULL;}
static int bus_dma_tag_create(bus_dma_tag_t p,unsigned a,unsigned b,uint64_t lo,uint64_t hi,void *f,void *fa,uint64_t size,int ns,uint64_t ms,int flags,void *lock,void *arg,bus_dma_tag_t *out) {(void)p;(void)a;(void)b;(void)lo;(void)hi;(void)f;(void)fa;(void)size;(void)ns;(void)ms;(void)flags;(void)lock;(void)arg;if(fault())return ENOMEM;assert(!physical_tag.alive);physical_tag.alive=true;*out=&physical_tag;return 0;}
static int bus_dma_tag_create_bcm2712_ram(bus_dma_tag_t p,const struct bcm2712_dma_policy *policy,unsigned a,unsigned b,uint64_t lo,uint64_t hi,uint64_t size,int ns,uint64_t ms,int flags,void *lock,void *arg,bus_dma_tag_t *out) {(void)policy;(void)a;(void)b;(void)lo;(void)hi;(void)size;(void)ns;(void)ms;(void)flags;(void)lock;(void)arg;assert(p==&physical_tag&&p->alive);if(fault())return ENOMEM;p->children++;translated_tag.alive=true;*out=&translated_tag;return 0;}
static int bus_dma_tag_destroy(bus_dma_tag_t t) {if(++cleanup_calls==cleanup_fail_at)return EBUSY;if(destroy_error)return destroy_error;assert(t->alive&&!t->children);if(t==&translated_tag)physical_tag.children--;t->alive=false;return 0;}
static int bus_get_domain(device_t d,int *domain) {(void)d;*domain=0;return domain_error;}
static int bus_dma_tag_set_domain(bus_dma_tag_t t,int domain) {(void)t;(void)domain;return fault()?EINVAL:0;}
static int rman_init(struct rman *r) {if(fault())return ENOMEM;r->initialized=true;return 0;}
static int rman_fini(struct rman *r) {if(++cleanup_calls==cleanup_fail_at)return EBUSY;assert(r->initialized);if(fini_error)return fini_error;r->initialized=false;return 0;}
static int rman_manage_region(struct rman *r,uint64_t first,uint64_t last) {assert(r->initialized);if(fault())return EIO;r->first=first;r->last=last;return 0;}
static int bus_get_resource(device_t d,int type,int rid,uint64_t *start,uint64_t *size) {(void)d;(void)type;(void)start;(void)size;if(resource_error)return resource_error;return entries[rid-100]?0:ENOENT;}
static int bus_set_resource(device_t d,int type,int rid,uint64_t start,uint64_t size) {(void)d;(void)type;if(fault())return EIO;assert(!entries[rid-100]);entries[rid-100]=true;resources[rid-99].start=start;resources[rid-99].size=size;return 0;}
static void bus_delete_resource(device_t d,int type,int rid) {(void)d;(void)type;assert(entries[rid-100]&&!resources[rid-99].allocated);entries[rid-100]=false;}
static void ofw_bus_setup_iinfo(int n,int *out,unsigned size) {(void)n;assert(size==4);*out=1;}
static void ofw_bus_destroy_iinfo(int *out) {assert(*out==1);*out=0;}
static uint32_t bus_read_4(struct resource *r,unsigned off) {assert(r->allocated&&off<0x9310);reads++;if(off>=0x8000&&off<0x9000)data_accesses++;return r->words[off/4];}
static uint32_t bus_read_2(struct resource *r,unsigned off) {return (bus_read_4(r,off&~3U)>>((off&2)*8))&0xffff;}
static uint32_t bus_read_1(struct resource *r,unsigned off) {return (bus_read_4(r,off&~3U)>>((off&3)*8))&0xff;}
static void bus_write_4(struct resource *r,unsigned off,uint32_t v) {assert(r->allocated&&off<0x9310);writes++;if(off==0x9000)index_writes++;if(off>=0x8000&&off<0x9000)data_accesses++;r->words[off/4]=v;}
static void bus_write_2(struct resource *r,unsigned off,uint32_t v) {uint32_t mask=0xffffU<<((off&2)*8);bus_write_4(r,off&~3U,(r->words[off/4]&~mask)|((v&0xffff)<<((off&2)*8)));}
static void bus_write_1(struct resource *r,unsigned off,uint32_t v) {uint32_t mask=0xffU<<((off&3)*8);bus_write_4(r,off&~3U,(r->words[off/4]&~mask)|((v&0xff)<<((off&3)*8)));}
static void bus_barrier(struct resource *r,unsigned off,unsigned width,unsigned flags) {(void)r;(void)off;(void)width;(void)flags;}
#define DELAY(n) ((void)(n))
static device_t device_add_child(device_t d,const char *s,int unit) {(void)d;(void)s;(void)unit;if(child_missing)return NULL;children++;return &pci;}
static int device_probe_and_attach(device_t d) {if(!child_error&&!child_unattached)d->attached=true;return child_error;}
static void device_disable(device_t d) {d->disabled=true;}
static int device_delete_child(device_t d,device_t c) {(void)d;if(delete_error)return delete_error;c->attached=false;children--;return 0;}
static struct resource *pci_host_generic_core_alloc_resource(device_t d,device_t c,int type,int rid,uint64_t first,uint64_t last,uint64_t count,unsigned flags) {(void)d;(void)c;(void)type;(void)rid;(void)first;(void)last;(void)count;(void)flags;delegates++;return &resources[1];}
static int bus_generic_setup_intr(device_t d,device_t c,struct resource *r,int f,driver_filter_t *filter,driver_intr_t *handler,void *arg,void **cookie) {(void)d;(void)c;(void)r;(void)f;(void)filter;(void)handler;(void)arg;(void)cookie;delegates++;return 0;}
static unsigned pci_get_rid(device_t d) {(void)d;return 0x100;}
static int ofw_bus_msimap(int node,unsigned rid,int *parent,void *id) {(void)node;(void)rid;(void)id;*parent=0x27;return 0;}
static int intr_alloc_msi(device_t d,device_t c,int p,int count,int max,int *irqs) {(void)d;(void)c;(void)p;(void)count;(void)max;(void)irqs;msi_allocations++;return 0;}
static int intr_release_msi(device_t d,device_t c,int p,int count,int *irqs) {(void)d;(void)c;(void)p;(void)count;(void)irqs;msi_releases++;return 0;}
static int intr_alloc_msix(device_t d,device_t c,int p,int *irq) {return intr_alloc_msi(d,c,p,1,1,irq);}
static int intr_release_msix(device_t d,device_t c,int p,int irq) {return intr_release_msi(d,c,p,1,&irq);}
static int intr_map_msi(device_t d,device_t c,int p,int irq,uint64_t *addr,uint32_t *data) {(void)d;(void)c;(void)p;(void)irq;*addr=mapped_address;*data=mapped_data;return 0;}
static void bus_generic_new_pass(device_t d) {(void)d;delegates++;}
static void bus_generic_driver_added(device_t d,driver_t *driver) {(void)d;(void)driver;delegates++;}

struct resource_map_request { uint64_t offset,length; };
struct resource_map { unsigned unused; };
static struct resource *mapped_backing;static uint64_t mapped_offset,mapped_length;
static int rman_get_type(struct resource *r) {return r->type;}
static unsigned rman_get_flags(struct resource *r) {return r->flags;}
static uint64_t rman_get_end(struct resource *r) {return r->start+(r->size-1);}
static void resource_init_map_request(struct resource_map_request *a) {*a=(struct resource_map_request){0};}
static int resource_validate_map_request(struct resource *r,struct resource_map_request *input,struct resource_map_request *a,uint64_t *start,uint64_t *length) {if(input)*a=*input;*start=r->start+a->offset;*length=a->length?a->length:r->size;return 0;}
static int bus_generic_map_resource(device_t d,device_t c,struct resource *r,struct resource_map_request *a,struct resource_map *m) {(void)d;(void)c;(void)r;(void)a;(void)m;return EOPNOTSUPP;}
static int bus_generic_unmap_resource(device_t d,device_t c,struct resource *r,struct resource_map *m) {(void)d;(void)c;(void)r;(void)m;return EOPNOTSUPP;}
static int bus_map_resource(device_t d,struct resource *r,struct resource_map_request *a,struct resource_map *m) {(void)d;(void)m;mapped_backing=r;mapped_offset=a->offset;mapped_length=a->length;return 0;}
static int bus_unmap_resource(device_t d,struct resource *r,struct resource_map *m) {(void)d;(void)m;assert(r==mapped_backing);mapped_backing=NULL;return 0;}
