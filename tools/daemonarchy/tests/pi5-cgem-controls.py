#!/usr/bin/env python3
"""Fault controls for production CGEM RP1 configuration and owner cleanup.

Native compilation and physical link/DMA/packet qualification remain separate.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'sys/dev/cadence/if_cgem.c'


def function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:int|void)\n' + name + r'\(', source)
    if match is None:
        raise ValueError(name)
    pos = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#define CGEM64
#include <if_cgem_hw.h>
#undef NULL
#define NULL 0
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define ETHER_ADDR_LEN 6
#define ETHER_IS_ZERO(a) (!memcmp(a,"\0\0\0\0\0\0",6))
#define ETHER_IS_MULTICAST(a) ((a)[0]&1)
#define bcopy(a,b,n) memcpy(b,a,n)
#define CGEM_NUM_RX_DESCS 4
#define CGEM_NUM_TX_DESCS 4
#define CGEM_GPIO_ACTIVE_LOW 1U
#define GPIO_PIN_OUTPUT 2
#define GPIO_PIN_PRESET_LOW 0x400
#define GPIO_PIN_PRESET_HIGH 0x800
#define MIIF_RX_DELAY 1
#define MIIF_TX_DELAY 2
#define MII_CONTYPE_RGMII 1
#define MII_CONTYPE_RGMII_ID 2
#define MII_CONTYPE_RGMII_RXID 3
#define MII_CONTYPE_RGMII_TXID 4
#define IFM_1000_T 1
#define IFM_100_TX 2
#define IFM_10_T 3
#define IFM_FDX 0x100
#define IFM_SUBTYPE(m) ((m)&0xff)
#define SYS_RES_MEMORY 1
#define SYS_RES_IRQ 2
#define IFF_UP 1
struct ifnet { int unused; };
struct ether_addr { uint8_t bytes[6]; };
typedef struct ifnet *if_t;
typedef int device_t;
typedef int phandle_t;
typedef uint32_t pcell_t;
typedef unsigned u_int;
typedef unsigned char u_char;
typedef uint64_t bus_addr_t;
typedef struct { uint64_t ds_addr,ds_len; } bus_dma_segment_t;
struct clock { unsigned id, held,enabled; uint64_t frequency; };
typedef struct clock *clk_t;
struct gpiobus_pin { uint32_t flags,pin; };
typedef struct gpiobus_pin *gpio_pin_t;
struct resource { unsigned rid; };
struct mii_data { unsigned mii_media_active; };
struct cgem_softc {
 device_t dev,miibus;
 if_t ifp;
 struct resource *mem_res,*irq_res;
 void *intrhand;
 int tick_ch,sc_mtx,neednullqs,rxhangwar,rxbufs,if_old_flags;
 bool rp1,lock_initialized,callout_initialized,ether_attached,hardware_initialized;
 bool desc_map_loaded,detaching,phy_reset_armed;
 unsigned clock_enabled;
 clk_t clk_pclk,clk_hclk,clk_txclk,clk_rxclk,clk_tsuclk;
 uint32_t mdc_div,rp1_axi_mask,rp1_axi_value;
 int phy_contype,phy_addr,phy_flags;
 gpio_pin_t phy_reset;
 uint32_t phy_reset_ms,phy_post_ms;
 void *desc_dma_tag,*mbuf_dma_tag;
 struct cgem_rx_desc *rxring;
 struct cgem_tx_desc *txring;
 void *null_qs,*rxring_dma_map;
 bus_addr_t rxring_physaddr,txring_physaddr,null_qs_physaddr;
 void *rxring_m_dmamap[CGEM_NUM_RX_DESCS],*txring_m_dmamap[CGEM_NUM_TX_DESCS];
 unsigned net_cfg_shadow,net_ctl_shadow,mii_media_active;
 void *rxring_m[4],*txring_m[4];
 unsigned txring_hd_ptr,txring_tl_ptr,txring_queued,rxring_hd_ptr,rxring_tl_ptr,rxring_queued;
};
static struct cgem_softc sc;
static struct mii_data mii;
static struct ifnet netif;
static struct resource mem={0},irq={0};
static struct clock clocks[5];
static struct gpiobus_pin reset_pin;
static struct cgem_rx_desc rx[4];
static struct cgem_tx_desc tx[4];
static struct cgem_rx_desc arena[16];
static uint32_t regs[0x4000/4];
static bool locked;
static unsigned writes,reads,clock_gets,clock_enables,clock_disables,clock_releases;
static unsigned set_rates,last_rate,last_clock,gpio_gets,gpio_sets,gpio_releases;
static unsigned preset_flags,delays[1500],ndelays;
static bool gpio_level,drained,ether_detached,child_detached,interface_freed;
static unsigned operation,fail_operation,unloaded,freed,tag_destroyed,mem_released;
static unsigned map_destroyed,irq_torn,packet_unloaded,packet_freed,desc_unloaded;
static bool tx_stuck,bad_readback,bad_rate,reset_present;
static int get_fail=-1,enable_fail=-1,disable_fail=-1,release_fail=-1,gpio_fail;
static int short_property,wrong_phy,zero_pipe;
static uint8_t mac_property[6],local_property[6];
static bool have_mac,have_local;
static unsigned random_mac,acquisitions,fail_acquire;
static int callback_error;
static bool desc_mapped,ether_published;
static bool acquire(void) {return ++acquisitions==fail_acquire;}
static const char *names[]={"pclk","hclk","tx_clk","rx_clk","tsu_clk"};
static int step(void) { return ++operation==fail_operation ? EBUSY:0; }
static void *device_get_softc(device_t d) { assert(d==1);return &sc; }
static void lock(void) { assert(sc.lock_initialized&&!locked);locked=true; }
static void unlock(void) { assert(locked);locked=false; }
#define CGEM_LOCK_INIT(s) ((void)(s))
#define CGEM_LOCK(s) lock()
#define CGEM_UNLOCK(s) unlock()
#define CGEM_ASSERT_LOCKED(s) assert(locked)
#define CGEM_LOCK_DESTROY(s) assert(!locked)
static uint32_t rd(unsigned off) {
 assert(sc.mem_res==&mem && off%4==0 && off<sizeof(regs));reads++;
 if(off==CGEM_TX_STAT && tx_stuck)return CGEM_TX_STAT_GO;
 uint32_t value=regs[off/4];
 if(bad_readback && (off==CGEM_USER_IO || off==CGEM_AXI_PIPELINE))value=0;
 return value;
}
static void wr(unsigned off,uint32_t value) {
 assert(sc.mem_res==&mem && off%4==0 && off<sizeof(regs));writes++;
 if(off==CGEM_TX_STAT)regs[off/4]&=~(value&~CGEM_TX_STAT_GO);
 else regs[off/4]=value;
}
#define RD4(s,o) rd(o)
#define WR4(s,o,v) wr(o,v)
static int device_printf(device_t d,const char *fmt,...) {assert(d==1&&fmt);return 0;}
static void DELAY(unsigned us) {assert(ndelays<nitems(delays));delays[ndelays++]=us;}
static int clk_get_by_ofw_name(device_t d,int node,const char *name,clk_t *out) {
 assert(d==1&&node==0);clock_gets++;if(acquire())return ENODEV;
 for(unsigned i=0;i<5;i++)if(!strcmp(names[i],name)) {
  if((int)i==get_fail)return ENODEV;
  *out=&clocks[i];clocks[i].held++;return 0;
 }
 abort();
}
static int clk_enable(clk_t c) {clock_enables++;if(acquire())return EIO;if((int)c->id==enable_fail)return EIO;c->enabled++;return 0;}
static int clk_disable(clk_t c) {
 clock_disables++;assert(c->enabled);
 if((int)c->id==disable_fail)return EBUSY;c->enabled--;return 0;
}
static int clk_release(clk_t c) {
 clock_releases++;assert(c->held&&!c->enabled);
 if((int)c->id==release_fail)return EBUSY;c->held--;return 0;
}
static int clk_set_freq(clk_t c,uint64_t rate,int flags) {
 assert(!locked&&flags==0);set_rates++;last_rate=rate;last_clock=c->id;
 c->frequency=bad_rate?rate+1:rate;return 0;
}
static int clk_get_freq(clk_t c,uint64_t *rate) {*rate=c->frequency;return 0;}
static phandle_t ofw_bus_get_node(device_t d) {assert(d==1);return 42;}
static phandle_t OF_parent(phandle_t p) {assert(p==43);return wrong_phy?44:42;}
static phandle_t OF_node_from_xref(phandle_t p) {return p==2?43:0;}
static bool ofw_bus_node_status_okay(phandle_t p) {return p==43;}
static bool OF_hasprop(phandle_t n,const char *name) {
 assert(n==42);
 if(!strcmp(name,"phy-reset-gpios"))return reset_present;
 return !strcmp(name,"cdns,ar2r-max-pipe")||!strcmp(name,"cdns,aw2w-max-pipe")||
  !strcmp(name,"cdns,use-aw2b-fill")||!strcmp(name,"phy-reset-duration");
}
static int OF_getproplen(phandle_t n,const char *name) {
 assert(n==42);
 if(!strcmp(name,"cdns,use-aw2b-fill"))return 0;
 return short_property?4:1;
}
static int OF_getprop(phandle_t n,const char *name,void *out,size_t len) {
 assert(n==42);
 if(!strcmp(name,"mac-address")||!strcmp(name,"local-mac-address")) {
  bool local=!strcmp(name,"local-mac-address");
  if(local?!have_local:!have_mac)return -1;
  assert(len==6);memcpy(out,local?local_property:mac_property,6);return 6;
 }
 assert(len==1);*(uint8_t*)out=zero_pipe?0:8;return 1;
}
static int OF_getencprop(phandle_t n,const char *name,void *out,size_t len) {
 assert(len==4);uint32_t value;
 if(n==42&&!strcmp(name,"phy-handle"))value=2;
 else if(n==43&&!strcmp(name,"reg"))value=1;
 else if(n==42&&!strcmp(name,"phy-reset-duration"))value=5;
 else abort();
 *(uint32_t*)out=value;return 4;
}
static void ether_gen_addr(if_t ifp,struct ether_addr *addr) {
 assert(ifp==&netif);uint8_t a[]={2,0xaa,0xbb,0xcc,0xdd,0xee};memcpy(addr,a,6);random_mac++;
}
static int gpio_pin_get_by_ofw_property(device_t d,phandle_t n,const char *name,gpio_pin_t *out) {
 assert(d==1&&n==42&&!strcmp(name,"phy-reset-gpios"));gpio_gets++;if(acquire())return ENODEV;
 if(gpio_fail==1)return ENODEV;*out=&reset_pin;return 0;
}
static int gpio_pin_setflags(gpio_pin_t p,unsigned flags) {
 assert(p==&reset_pin);if(acquire())return EIO;preset_flags=flags;gpio_level=!!(flags&GPIO_PIN_PRESET_HIGH);gpio_sets++;
 return gpio_fail==2?EIO:0;
}
static int gpio_pin_set_active(gpio_pin_t p,bool active) {
 assert(p==&reset_pin);gpio_sets++;
 if(acquire()||gpio_fail==3)return EBUSY;gpio_level=active!=!!(p->flags&CGEM_GPIO_ACTIVE_LOW);return 0;
}
static void gpio_pin_release(gpio_pin_t p) {assert(p==&reset_pin);gpio_releases++;}
static void callout_stop(int *c) {assert(c==&sc.tick_ch&&locked);}
static void m_freem(void *p) {assert(p==rx||p==tx);packet_freed++;}
static void callout_drain(int *c) {assert(c==&sc.tick_ch);drained=true;}
static int bus_teardown_intr(device_t d,struct resource *r,void *cookie) {
 assert(d==1&&r==&irq&&cookie==&sc);int e=step();if(!e)irq_torn++;return e;
}
static void if_setflagbits(if_t p,int set,int clear) {assert(p==&netif&&set==0&&clear==IFF_UP);}
static void ether_ifdetach(if_t p) {assert(p==&netif&&drained);ether_detached=true;}
static int bus_generic_detach(device_t d) {assert(d==1);int e=step();if(!e)child_detached=true;return e;}
static int bus_dmamap_destroy(void *tag,void *map) {
 assert(child_detached&&tag==&mii&&map);int e=step();if(!e)map_destroyed++;return e;
}
static void bus_dmamap_unload(void *tag,void *map) {
 assert((tag==&sc&&(map==rx||map==arena))||(tag==&mii&&(map==rx||map==tx)));
 unloaded++;if(tag==&sc){assert(desc_mapped);desc_mapped=false;desc_unloaded++;}else packet_unloaded++;
}
static void bus_dmamem_free(void *tag,void *address,void *map) {
 assert(tag==&sc&&(address==rx||address==arena)&&map==address&&!desc_mapped);freed++;
}
static int bus_dma_tag_destroy(void *tag) {
 assert(tag==&sc||tag==&mii);int e=step();if(!e)tag_destroyed++;return e;
}
static void if_free(if_t p) {assert(p==&netif&&child_detached);interface_freed=true;}
static int rman_get_rid(struct resource *r) {assert(r==&mem||r==&irq);return 0;}
static int bus_release_resource(device_t d,int type,int rid,struct resource *r) {
 assert(d==1&&rid==0&&((type==SYS_RES_IRQ&&r==&irq)||(type==SYS_RES_MEMORY&&r==&mem)));
 assert(!sc.clock_enabled&&!sc.phy_reset&&!sc.intrhand);int e=step();if(!e)mem_released++;return e;
}

#define BUS_SPACE_MAXADDR UINT64_MAX
#define BUS_DMA_NOWAIT 1
#define BUS_DMA_COHERENT 2
#define BUS_DMA_ZERO 4
#define MCLBYTES 2048
#define TX_MAX_DMA_SEGS 8
#define RF_ACTIVE 1
#define IFT_ETHER 1
#define IF_CGEM_NAME "cgem"
#define IFF_BROADCAST 2
#define IFF_SIMPLEX 4
#define IFF_MULTICAST 8
#define IFCAP_HWCSUM 1
#define IFCAP_HWCSUM_IPV6 2
#define IFCAP_VLAN_MTU 4
#define IFCAP_VLAN_HWCSUM 8
#define BMSR_DEFCAPMASK 0xffff
#define MII_PHY_ANY -1
#define MII_OFFSET_ANY -1
#define DEFAULT_NUM_RX_BUFS 2
#define INTR_TYPE_NET 1
#define INTR_MPSAFE 2
#define INTR_EXCL 4
struct ofw_compat_data { const char *ocd_str; uintptr_t ocd_data; };
static const struct ofw_compat_data *ofw_bus_search_compatible(device_t d,struct ofw_compat_data *data) {
 assert(d==1);for(;data->ocd_str;data++)if(!strcmp(data->ocd_str,"raspberrypi,rp1-gem"))return data;
 abort();
}
static int mii_fdt_get_contype(phandle_t n) {assert(n==42);return MII_CONTYPE_RGMII_ID;}
static unsigned device_get_unit(device_t d) {assert(d==1);return 0;}
static void callout_init_mtx(int *c,int *m,int flags) {assert(c==&sc.tick_ch&&m==&sc.sc_mtx&&flags==0);}
static struct resource *bus_alloc_resource_any(device_t d,int type,int *rid,int flags) {
 assert(d==1&&*rid==0&&flags==RF_ACTIVE);if(acquire())return NULL;
 return type==SYS_RES_MEMORY?&mem:&irq;
}
static uint64_t rman_get_size(struct resource *r) {assert(r==&mem);return 0x4000;}
static if_t if_alloc(int type) {assert(type==IFT_ETHER);return acquire()?NULL:&netif;}
static void if_setsoftc(if_t p,void *softc) {assert(p==&netif&&softc==&sc);}
#define if_initname(p,n,u) assert((p)==&netif&&!strcmp(n,"cgem")&&(u)==0)
#define if_setflags(p,f) ((void)(p),(void)(f))
#define if_setinitfn(p,f) ((void)(p),(void)(f))
#define if_setioctlfn(p,f) ((void)(p),(void)(f))
#define if_setstartfn(p,f) ((void)(p),(void)(f))
#define if_setcapabilitiesbit(p,f,g) ((void)(p),(void)(f),(void)(g))
#define if_setsendqlen(p,l) ((void)(p),(void)(l))
#define if_setsendqready(p) ((void)(p))
#define if_sethwassist(p,a) ((void)(p),(void)(a))
#define if_setcapenable(p,c) ((void)(p),(void)(c))
#define if_getcapabilities(p) 15
#define if_getflags(p) 14
static void cgem_init(void) {}
static void cgem_ioctl(void) {}
static void cgem_start(void) {}
static void cgem_intr(void *p) {(void)p;}
static void cgem_ifmedia_upd(void) {}
static void cgem_ifmedia_sts(void) {}
static int mii_attach(device_t d,device_t *bus,if_t p,void *upd,void *sts,unsigned cap,int addr,int offset,int flags) {
 assert(d==1&&p==&netif&&upd&&sts&&cap==0xffff&&addr==1&&offset==-1&&flags==(MIIF_RX_DELAY|MIIF_TX_DELAY));
 if(acquire())return ENXIO;*bus=2;return 0;
}
static void *bus_get_dma_tag(device_t d) {assert(d==1);return regs;}
static void busdma_lock_mutex(void *p,int op) {(void)p;(void)op;}
static int bus_dma_tag_create(void *parent,unsigned align,uint64_t boundary,uint64_t low,uint64_t high,
 void *filter,void *arg,uint64_t maxsize,int nseg,uint64_t maxseg,int flags,void (*lockfn)(void*,int),void *lockarg,void **out) {
 assert(parent==regs&&align==1&&low==UINT64_MAX&&high==UINT64_MAX&&!filter&&!arg&&flags==0);
 assert(lockfn==busdma_lock_mutex&&lockarg==&sc.sc_mtx&&maxsize>=maxseg);
 if(acquire())return ENOMEM;
 if(boundary) {assert(boundary==(1ULL<<32)&&nseg==1);*out=&sc;}
 else {assert(nseg==8);*out=&mii;}
 return 0;
}
static int bus_dmamem_alloc(void *tag,void **out,int flags,void **map) {
 assert(tag==&sc&&flags==7);if(acquire())return ENOMEM;
 memset(arena,0,sizeof(arena));*out=arena;*map=arena;return 0;
}
static int bus_dmamap_load(void *tag,void *map,void *buf,int size,
 void (*fn)(void*,bus_dma_segment_t*,int,int),void *arg,int flags) {
 assert(tag==&sc&&map==arena&&buf==arena&&size<=(int)sizeof(arena)&&flags==1);
 if(acquire())return ENOMEM;
 bus_dma_segment_t seg={UINT64_C(0x1000000000),size};
 desc_mapped=callback_error!=1;
 fn(arg,&seg,callback_error==2?2:1,callback_error==1?EIO:0);return 0;
}
static int bus_setup_intr(device_t d,struct resource *r,int flags,void *filter,
 void (*handler)(void*),void *arg,void **cookie) {
 assert(d==1&&r==&irq&&flags==7&&!filter&&handler==cgem_intr&&arg==&sc);
 if(acquire())return ENXIO;*cookie=&sc;return 0;
}
static void ether_ifattach(if_t p,uint8_t *mac) {
 assert(p==&netif&&sc.intrhand==&sc&&!ETHER_IS_ZERO(mac)&&!ETHER_IS_MULTICAST(mac));ether_published=true;
}
static void cgem_add_sysctls(device_t d) {assert(d==1);}
static int cgem_detach(device_t);
'''

MAIN = r'''
static void reset(void) {
 memset(&sc,0,sizeof(sc));memset(clocks,0,sizeof(clocks));memset(regs,0,sizeof(regs));
 sc.dev=1;sc.mem_res=&mem;sc.lock_initialized=true;sc.ifp=&netif;
 sc.phy_contype=MII_CONTYPE_RGMII_ID;
 sc.clk_pclk=&clocks[0];sc.clk_hclk=&clocks[1];sc.clk_txclk=&clocks[2];sc.clk_tsuclk=&clocks[4];
 for(unsigned i=0;i<5;i++){clocks[i].id=i;clocks[i].frequency=200000000;}
 clocks[2].frequency=125000000;clocks[4].frequency=50000000;
 regs[CGEM_DESIGN_CFG6/4]=CGEM_DESIGN_CFG6_ADDR_64B;
 reset_pin.flags=1;reset_pin.pin=32;
 writes=reads=clock_gets=clock_enables=clock_disables=clock_releases=0;
 set_rates=last_rate=last_clock=gpio_gets=gpio_sets=gpio_releases=0;
 preset_flags=ndelays=operation=fail_operation=unloaded=freed=tag_destroyed=mem_released=0;
 map_destroyed=irq_torn=packet_unloaded=packet_freed=desc_unloaded=random_mac=0;
 acquisitions=fail_acquire=0;callback_error=0;desc_mapped=ether_published=false;
 locked=gpio_level=drained=ether_detached=child_detached=interface_freed=false;
 tx_stuck=bad_readback=bad_rate=reset_present=false;
 get_fail=enable_fail=disable_fail=release_fail=-1;gpio_fail=0;
 short_property=wrong_phy=zero_pipe=0;have_mac=have_local=false;
 memset(mac_property,0,6);memset(local_property,0,6);
}
static void full_owner(void) {
 reset();sc.irq_res=&irq;sc.intrhand=&sc;sc.hardware_initialized=true;
 sc.callout_initialized=sc.ether_attached=true;
 sc.desc_dma_tag=&sc;sc.mbuf_dma_tag=&mii;
 sc.rxring=rx;sc.txring=tx;sc.rxring_dma_map=rx;sc.desc_map_loaded=desc_mapped=true;
 sc.rxring_physaddr=0; /* Zero is a valid DMA address and still must unload. */
 sc.rxring_m_dmamap[0]=rx;sc.txring_m_dmamap[1]=tx;
 sc.rxring_m[0]=rx;sc.txring_m[1]=tx;
 sc.phy_reset=&reset_pin;sc.phy_reset_armed=true;
 for(unsigned i=0;i<5;i++){clocks[i].held=clocks[i].enabled=1;}
 sc.clk_rxclk=&clocks[3];sc.clock_enabled=31;
}
int main(void) {
 uint32_t divider;
 uint8_t mac[6];uint64_t frequency;
 reset();
 assert(cgem_rp1_mdc(200000000,&divider)==0&&divider==CGEM_NET_CFG_MDC_CLK_DIV_96);
 assert(cgem_rp1_mdc(120000000,&divider)==0&&divider==CGEM_NET_CFG_MDC_CLK_DIV_48);
 assert(cgem_rp1_mdc(560000000,&divider)==0&&divider==CGEM_NET_CFG_MDC_CLK_DIV_224);
 assert(cgem_rp1_mdc(560000001,&divider)==ERANGE);
 assert(cgem_rp1_mdc(0,&divider)==EINVAL);
 assert(cgem_rp1_parse(&sc)==0 && sc.phy_flags==(MIIF_RX_DELAY|MIIF_TX_DELAY));
 assert(sc.phy_addr==1&&sc.rp1_axi_mask==0x1ffff&&sc.rp1_axi_value==0x10808&&sc.phy_reset_ms==5);
 regs[CGEM_AXI_PIPELINE/4]=0xcafe0000;
 assert(cgem_rp1_setup(&sc)==0 && sc.mdc_div==CGEM_NET_CFG_MDC_CLK_DIV_96);
 assert(regs[CGEM_AXI_PIPELINE/4]==((0xcafe0000&~0x1ffffU)|0x10808));
 assert(regs[CGEM_USER_IO/4]&CGEM_USER_IO_RGMII);
 assert(last_rate==125000000&&last_clock==2);
 assert(clk_get_freq(sc.clk_pclk,&frequency)==0&&frequency==200000000);
 reset();regs[CGEM_DESIGN_CFG6/4]=0;
 assert(cgem_rp1_setup(&sc)==EOPNOTSUPP&&writes==0&&set_rates==0);
 reset();bad_readback=true;assert(cgem_rp1_setup(&sc)==EIO);
 reset();bad_rate=true;assert(cgem_rp1_setup(&sc)==ERANGE&&writes==0);
 reset();short_property=1;assert(cgem_rp1_parse(&sc)==EINVAL&&writes==0);
 reset();wrong_phy=1;assert(cgem_rp1_parse(&sc)==EINVAL&&writes==0);
 reset();zero_pipe=1;assert(cgem_rp1_parse(&sc)==EINVAL&&writes==0);
 reset();sc.phy_contype=99;assert(cgem_rp1_parse(&sc)==EOPNOTSUPP&&writes==0);
 for(unsigned low=0;low<2;low++) {
  reset();reset_present=true;reset_pin.flags=low;
  assert(cgem_rp1_parse(&sc)==0&&cgem_rp1_phy_reset(&sc)==0);
  assert(preset_flags==(GPIO_PIN_OUTPUT|(low?GPIO_PIN_PRESET_LOW:GPIO_PIN_PRESET_HIGH)));
  assert(gpio_level==!!low&&!sc.phy_reset_armed&&ndelays==2&&delays[0]==5000&&delays[1]==10000);
 }
 for(int fail=1;fail<=3;fail++) {
  reset();reset_present=true;gpio_fail=fail;cgem_rp1_parse(&sc);
  assert(cgem_rp1_phy_reset(&sc)!=0);
  assert(sc.phy_reset_armed==(fail!=1));
 }
 for(unsigned i=0;i<5;i++) {
  reset();clk_t c=NULL;get_fail=i;
  assert(cgem_clock_get(&sc,names[i],&c,i,true)==ENODEV&&!c&&!sc.clock_enabled);
  get_fail=-1;enable_fail=i;
  assert(cgem_clock_get(&sc,names[i],&c,i,true)==EIO&&c==&clocks[i]&&!sc.clock_enabled);
 }
 reset();get_fail=0;clk_t absent=NULL;
 assert(cgem_clock_get(&sc,"pclk",&absent,0,false)==0&&!absent);
 for(int fail=0;fail<5;fail++) {
  full_owner();disable_fail=fail;
  assert(cgem_clocks_release(&sc)==EBUSY);
  assert(clocks[fail].held==1&&clocks[fail].enabled==1);
  disable_fail=-1;assert(cgem_clocks_release(&sc)==0&&!sc.clock_enabled);
  full_owner();release_fail=fail;
  assert(cgem_clocks_release(&sc)==EBUSY);
  assert(clocks[fail].held==1&&clocks[fail].enabled==0);
  release_fail=-1;assert(cgem_clocks_release(&sc)==0);
 }
 /* Valid FDT overrides register fallback; multicast/zero data never wins. */
 reset();regs[CGEM_SPEC_ADDR_LOW(0)/4]=0xccbbaa03;regs[CGEM_SPEC_ADDR_HI(0)/4]=0xeedd;
 regs[CGEM_SPEC_ADDR_LOW(1)/4]=0xccbbaa02;regs[CGEM_SPEC_ADDR_HI(1)/4]=0xeedd;
 cgem_get_mac(&sc,mac);assert(mac[0]==2&&mac[3]==0xcc&&!random_mac);
 have_mac=true;memcpy(mac_property,"\x02\x11\x22\x33\x44\x55",6);
 cgem_get_mac(&sc,mac);assert(!memcmp(mac,mac_property,6));
 reset();cgem_get_mac(&sc,mac);assert(random_mac==1&&mac[0]==2);
 /* RP1 media changes keep both SYS200MHz and TX125MHz constant. */
 for(unsigned rp1=0;rp1<2;rp1++) {
  reset();sc.rp1=rp1;lock();
  const unsigned media[]={IFM_1000_T,IFM_100_TX,IFM_10_T};
  const unsigned rates[]={125000000,25000000,2500000};
  for(unsigned i=0;i<3;i++) {
   mii.mii_media_active=media[i]|IFM_FDX;cgem_mediachange(&sc,&mii);
   assert(locked&&sc.mii_media_active==mii.mii_media_active);
   if(rp1)assert(set_rates==0&&clocks[0].frequency==200000000&&clocks[2].frequency==125000000);
   else assert(set_rates==i+1&&last_clock==0&&last_rate==rates[i]);
  }
  unlock();
 }
 struct cgem_dma_load load={0};bus_dma_segment_t segment={UINT64_C(0x1000001234),64};
 cgem_getaddr(&load,&segment,1,0);assert(load.called&&load.mapped&&!load.error&&load.address==segment.ds_addr);
 memset(&load,0,sizeof(load));cgem_getaddr(&load,&segment,1,EIO);assert(load.called&&!load.mapped&&load.error==EIO);
 memset(&load,0,sizeof(load));cgem_getaddr(&load,&segment,2,0);assert(load.called&&load.mapped&&load.error==EFBIG);
 full_owner();assert(cgem_detach(1)==0&&!sc.mem_res&&!sc.irq_res&&!sc.ifp&&!sc.lock_initialized);
 assert(irq_torn==1&&packet_freed==2&&packet_unloaded==2&&map_destroyed==2&&unloaded==3&&freed==1&&tag_destroyed==2);
 assert(ether_detached&&interface_freed&&gpio_releases==1&&mem_released==2);
 /* Every fallible resource release retains its owner and can be retried. */
 unsigned total=operation;
 for(unsigned fail=1;fail<=total;fail++) {
  full_owner();fail_operation=fail;
  assert(cgem_detach(1)==EBUSY&&sc.lock_initialized);
  fail_operation=0;assert(cgem_detach(1)==0&&!sc.mem_res&&!sc.lock_initialized);
 }
 full_owner();tx_stuck=true;assert(cgem_detach(1)==ETIMEDOUT&&sc.hardware_initialized);
 assert(!freed&&!unloaded&&!irq_torn&&!clock_disables&&sc.mem_res&&sc.intrhand);
 tx_stuck=false;assert(cgem_detach(1)==0);
 full_owner();gpio_fail=3;assert(cgem_detach(1)==EBUSY&&sc.phy_reset&&sc.phy_reset_armed);
 assert(!clock_disables&&sc.mem_res);gpio_fail=0;assert(cgem_detach(1)==0);
 /* Exercise the production attachment and descriptor setup at every acquire failure. */
 reset();memset(&sc,0,sizeof(sc));reset_present=true;
 assert(cgem_attach(1)==0&&sc.ether_attached&&ether_published&&sc.desc_map_loaded);
 assert(sc.rp1&&sc.neednullqs&&sc.mdc_div==CGEM_NET_CFG_MDC_CLK_DIV_96);
 unsigned count=acquisitions;assert(cgem_detach(1)==0);
 for(unsigned fail=1;fail<=count;fail++) {
  reset();memset(&sc,0,sizeof(sc));reset_present=true;fail_acquire=fail;
  int result=cgem_attach(1);
  if(fail==7) { /* Missing optional rx_clk is supported. */
   assert(result==0&&ether_published&&!sc.clk_rxclk);assert(cgem_detach(1)==0);
  } else assert(result!=0&&!ether_published);
  assert(!sc.ifp&&!sc.mem_res&&!sc.irq_res&&!sc.lock_initialized&&!sc.clock_enabled&&!sc.phy_reset);
  assert(!desc_mapped);
  for(unsigned i=0;i<5;i++)assert(!clocks[i].held&&!clocks[i].enabled);
 }
 reset();memset(&sc,0,sizeof(sc));reset_present=true;fail_acquire=count;fail_operation=1;
 assert(cgem_attach(1)==0&&sc.detaching&&!ether_published&&sc.mem_res&&sc.ifp);
 fail_acquire=fail_operation=0;assert(cgem_detach(1)==0&&!sc.mem_res&&!sc.ifp);
 for(int mode=1;mode<=2;mode++) {
  reset();memset(&sc,0,sizeof(sc));reset_present=true;callback_error=mode;
  assert(cgem_attach(1)!=0&&!desc_mapped&&!sc.rxring&&!sc.lock_initialized);
 }
 /* RP1 priority queue masks can contain gaps: later queues still get null rings. */
 reset();sc.null_qs=arena;sc.null_qs_physaddr=0x1000;
 regs[CGEM_DESIGN_CFG6/4]=(1U<<1)|(1U<<3);cgem_null_qs(&sc);
 assert(regs[CGEM_RX_QN_BAR(1)/4]==0x1000&&regs[CGEM_RX_QN_BAR(3)/4]==0x1000);
 assert(regs[CGEM_RX_QN_BAR(2)/4]==0);
 puts("PASS: RP1 CGEM fixed clock, MDIO, PHY reset/delays, AXI, MAC, DMA callback and fault-injected detach");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = SOURCE.read_text()
    helpers = '\n'.join(function(source, name) for name in (
        'cgem_clock_get', 'cgem_clocks_release', 'cgem_rp1_mdc',
        'cgem_rp1_parse', 'cgem_rp1_setup', 'cgem_rp1_phy_reset',
        'cgem_get_mac', 'cgem_mediachange', 'cgem_getaddr',
        'cgem_reset', 'cgem_stop', 'cgem_quiesce', 'cgem_detach',
        'cgem_null_qs', 'cgem_setup_descs', 'cgem_attach'))
    load = source[source.index('struct cgem_dma_load {'):source.index('/* bus_dmamap_load() may')]
    fdt = function((ROOT / 'sys/dev/fdt/fdt_common.c').read_text(), 'fdt_ether_get_addr')
    with tempfile.TemporaryDirectory(prefix='pi5-cgem-controls-') as tmp:
        path = Path(tmp)
        constants = '\n'.join(re.findall(r'(?m)^#define HWQUIRK_.*$', source))
        compat = source[source.index('static struct ofw_compat_data compat_data[]'):source.index('struct cgem_softc {')]
        (path / 'controls.c').write_text(PREFIX + constants + '\n' + compat + load + fdt + helpers + MAIN)
        subprocess.run([args.cc, '-std=gnu17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-sign-compare',
                        '-Wno-misleading-indentation', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I', str(ROOT / 'sys/dev/cadence'),
                        str(path / 'controls.c'), '-o', str(path / 'controls')], check=True)
        subprocess.run([str(path / 'controls')], check=True)


if __name__ == '__main__':
    main()
