#!/usr/bin/env python3
"""Production GENET frontend controls; no live PHY, DMA or Ethernet qualification."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[3]


def function(name):
    source = (ROOT/'sys/arm64/broadcom/genet/if_genet.c').read_text()
    start = re.search(r'(?m)^static (?:int|bool|void)\n'+name+r'\(', source).start()
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}')
        end += 1
    return source[start:end]


PREFIX = r'''
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/types.h>
#include <endian.h>
#define FDT 1
#define DEV_ACPI 1
#define __BIT(n) (1U<<(n))
#include "if_genetreg.h"
typedef int device_t;
typedef unsigned ACPI_HANDLE;
typedef int ACPI_STATUS;
typedef int phandle_t;
#define ETHER_ADDR_LEN 6
#define ETHER_IS_MULTICAST(a) ((a)[0]&1)
#define DEVICE_PROP_BUFFER 1
#define BUS_PROBE_DEFAULT -20
#define AE_OK 0
#define AE_NULL_ENTRY 1
#define AE_NOT_FOUND 2
#define AE_ERROR 3
#define ACPI_FAILURE(s) ((s)!=AE_OK)
#define TX_DESC_COUNT 4
#define RX_DESC_COUNT 4
enum { _RES_MAC, _RES_IRQ1, _RES_IRQ2 };
struct ether_addr { unsigned char octet[6]; };
struct ring { void *map; };
struct gen_softc {
 device_t dev;bool acpi;int type;mii_contype_t phy_mode;
 void *res[3],*ih,*ih2,*tx_buf_tag,*rx_buf_tag,*ifp;
 int mtx,stat_ch,link_task;
 struct ring tx_ring_ent[4],rx_ring_ent[4];
};
static struct gen_softc sc;
static int bootverbose;
static const char *property;
static ssize_t property_length;
static unsigned of_calls,property_calls,attaches,disabled,id_calls,desc_calls;
static unsigned dma_owner,parent_failure,lookup_failure,cycle;
static unsigned tears,drains,deletes,map_destroys,tag_destroys,freeifs,releases;
static uint32_t mac0,mac1,flush;
static mii_contype_t fdt_mode;
static int attach_error,of_mac_length;
static void *taskqueue_swi;
static int gen_spec[1];
static char *gen_acpi_ids[]={"BCM6E4E",NULL};
static void *device_get_softc(device_t d) {assert(d==1);return &sc;}
static device_t device_get_parent(device_t d) {assert(d==1);return 2;}
static int device_printf(device_t d,const char *fmt,...) {(void)fmt;assert(d==1);return 0;}
static int acpi_disabled(const char *s) {assert(!strcmp(s,"genet"));return disabled;}
static int id_probe(device_t parent,device_t d,char **ids,void *hid) {assert(parent==2&&d==1&&!strcmp(ids[0],"BCM6E4E")&&ids[1]==NULL&&hid==NULL);id_calls++;return attach_error;}
#define ACPI_ID_PROBE id_probe
static void device_set_desc(device_t d,const char *desc) {assert(d==1&&strstr(desc,"ACPI"));desc_calls++;}
static ACPI_HANDLE acpi_get_handle(device_t d) {assert(d==1);return 1;}
static ACPI_STATUS AcpiGetParent(ACPI_HANDLE h,ACPI_HANDLE *out) {if(parent_failure)return AE_ERROR;if(cycle){*out=1;return AE_OK;}if(h==3)return AE_NULL_ENTRY;*out=h+1;return AE_OK;}
static ACPI_STATUS AcpiGetHandle(ACPI_HANDLE h,const char *name,ACPI_HANDLE *out) {(void)out;assert(!strcmp(name,"_DMA"));if(lookup_failure)return AE_ERROR;return h==dma_owner?AE_OK:AE_NOT_FOUND;}
static int gen_attach(device_t d) {assert(d==1&&sc.acpi&&sc.type==5);attaches++;return attach_error;}
static ssize_t device_get_property(device_t d,const char *name,void *out,size_t len,int type) {assert(d==1&&!strcmp(name,"phy-mode")&&type==DEVICE_PROP_BUFFER);property_calls++;if(property_length>0&&property!=NULL)memcpy(out,property,(size_t)property_length<len?(size_t)property_length:len);return property_length;}
static phandle_t ofw_bus_get_node(device_t d) {assert(d==1);of_calls++;return 10;}
static mii_contype_t mii_fdt_get_contype(phandle_t n) {assert(n==10);of_calls++;return fdt_mode;}
static int OF_getprop(phandle_t n,const char *name,void *out,int len) {assert(n==10&&len==6);of_calls++;if(!strcmp(name,"mac-address")&&of_mac_length==6){memcpy(out,"\x02\x11\x22\x33\x44\x55",6);return 6;}return -1;}
static uint32_t read_reg(struct gen_softc *s,unsigned reg) {assert(s==&sc);if(reg==GENET_SYS_RBUF_FLUSH_CTRL)return flush;if(reg==GENET_UMAC_MAC0)return mac0;assert(reg==GENET_UMAC_MAC1);return mac1;}
#define RD4 read_reg
static bool mtx_initialized(int *m) {return *m!=0;}
static void mtx_destroy(int *m) {assert(*m);*m=0;}
static void bus_teardown_intr(device_t d,void *res,void *ih) {assert(d==1&&res&&ih);tears++;}
static void callout_drain(int *c) {assert(c==&sc.stat_ch&&sc.mtx);drains++;}
static void taskqueue_drain(void *q,int *t) {assert(q==taskqueue_swi&&t==&sc.link_task&&sc.mtx);drains++;}
static void device_delete_children(device_t d) {assert(d==1);deletes++;}
static int bus_dmamap_destroy(void *tag,void *map) {assert(tag&&map);map_destroys++;return 0;}
static int bus_dma_tag_destroy(void *tag) {assert(tag);tag_destroys++;return 0;}
static void bus_release_resources(device_t d,int *spec,void **res) {assert(d==1&&spec==gen_spec&&res==sc.res);releases++;}
static void if_free(void *p) {assert(p);freeifs++;}
'''
TEST = r'''
static void reset(void) {
 memset(&sc,0,sizeof(sc));sc.dev=1;property=NULL;property_length=-1;
 of_calls=property_calls=attaches=disabled=id_calls=desc_calls=0;
 dma_owner=parent_failure=lookup_failure=cycle=0;
 tears=drains=deletes=map_destroys=tag_destroys=freeifs=releases=0;
 mac0=0x02112233;mac1=0x4455;flush=0;attach_error=0;of_mac_length=-1;
 fdt_mode=MII_CONTYPE_RGMII_ID;
}
int main(void) {
 reset();assert(gen_acpi_probe(1)==BUS_PROBE_DEFAULT&&id_calls==1&&desc_calls==1);
 disabled=1;assert(gen_acpi_probe(1)==ENXIO&&id_calls==1);
 reset();attach_error=ENXIO;assert(gen_acpi_probe(1)==ENXIO&&desc_calls==0);
 reset();assert(gen_acpi_attach(1)==0&&attaches==1&&of_calls==0);
 reset();dma_owner=4;assert(gen_acpi_attach(1)==0); /* sibling DMA bus is irrelevant */
 reset();dma_owner=2;assert(gen_acpi_attach(1)==EOPNOTSUPP&&attaches==0);
 reset();parent_failure=1;assert(gen_acpi_attach(1)==ENXIO&&attaches==0);
 reset();lookup_failure=1;assert(gen_acpi_attach(1)==EOPNOTSUPP&&attaches==0);
 reset();cycle=1;assert(gen_acpi_attach(1)==E2BIG&&attaches==0);
 reset();attach_error=EIO;assert(gen_acpi_attach(1)==EIO&&attaches==1);
 const char *modes[]={"rgmii","rgmii-id","rgmii-rxid","rgmii-txid"};
 mii_contype_t values[]={MII_CONTYPE_RGMII,MII_CONTYPE_RGMII_ID,MII_CONTYPE_RGMII_RXID,MII_CONTYPE_RGMII_TXID};
 for(unsigned i=0;i<4;i++) {reset();sc.acpi=true;property=modes[i];property_length=strlen(property);assert(gen_get_phy_mode(1)==0&&sc.phy_mode==values[i]&&of_calls==0);}
 reset();sc.acpi=true;assert(gen_get_phy_mode(1)==ENXIO);
 property="mii";property_length=3;assert(gen_get_phy_mode(1)==ENXIO);
 property="rgmii\0bad";property_length=9;assert(gen_get_phy_mode(1)==ENXIO);
 property="rgmii-id but too long to fit the property buffer";property_length=strlen(property);assert(gen_get_phy_mode(1)==ENXIO);
 reset();assert(gen_get_phy_mode(1)==0&&sc.phy_mode==MII_CONTYPE_RGMII_ID&&property_calls==0&&of_calls==2);
 struct ether_addr mac;
 reset();sc.acpi=true;assert(gen_get_eaddr(1,&mac)&&of_calls==0&&!memcmp(mac.octet,"\x02\x11\x22\x33\x44\x55",6));
 mac0=0x01112233;assert(!gen_get_eaddr(1,&mac));
 mac0=mac1=0;assert(!gen_get_eaddr(1,&mac));
 reset();sc.acpi=true;flush=GENET_SYS_RBUF_FLUSH_RESET;assert(!gen_get_eaddr(1,&mac));
 reset();of_mac_length=6;assert(gen_get_eaddr(1,&mac)&&of_calls==2);
 reset();gen_destroy(&sc);assert(!tears&&!drains&&!map_destroys&&releases==1);
 reset();sc.mtx=1;sc.ih=(void *)1;sc.res[_RES_IRQ1]=(void *)2;sc.tx_buf_tag=(void *)3;sc.tx_ring_ent[1].map=(void *)4;sc.ifp=(void *)5;
 gen_destroy(&sc);assert(tears==1&&drains==2&&map_destroys==1&&tag_destroys==1&&freeifs==1&&!sc.mtx&&!sc.ih&&!sc.ifp&&!sc.tx_buf_tag);
 puts("PASS: production GENET ACPI probe, DMA ancestor checks, PHY properties, MAC preservation, FDT paths and partial-attach cleanup");return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    enum = re.search(r'enum mii_contype \{.*?typedef enum mii_contype mii_contype_t;',
                     (ROOT/'sys/dev/mii/miivar.h').read_text(), re.S).group()
    names = ['gen_get_phy_mode','gen_get_eaddr','gen_acpi_probe','gen_acpi_attach',
             'gen_bus_dma_teardown','gen_destroy']
    with tempfile.TemporaryDirectory(prefix='genet-acpi-') as tmp:
        source = Path(tmp)/'control.c'
        source.write_text(enum+'\n'+PREFIX+'\n'+'\n'.join(function(n) for n in names)+TEST)
        binary = Path(tmp)/'control'
        subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-sign-compare',
                        '-fsanitize=address,undefined','-I'+str(ROOT/'sys/arm64/broadcom/genet'),
                        str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    main()
