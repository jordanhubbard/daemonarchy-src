#!/usr/bin/env python3
"""Compile production pinctrl/parser and regulator callbacks against bounded fakes.

This exercises actual authored function bodies, not a second implementation.
It does not qualify Newbus attachment, hardware MMIO, or electrical timing.
"""
import argparse
import json
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[3]


def function(path, name):
    text = (ROOT / path).read_text()
    start = re.search(r'(?m)^static int\s+' + re.escape(name) + r'\(', text).start()
    body = text.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PIN = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bcm2712_pinctrl_policy.h"
typedef int phandle_t;
typedef int device_t;
typedef uint32_t pcell_t;
#define BCM2712_MAX_OPS 128
struct bcm2712_pinctrl_softc { int aon,lock; uint32_t *mem; };
static struct bcm2712_pinctrl_softc *device_sc;
static void *device_get_softc(device_t dev) { assert(dev==3);return device_sc; }
static phandle_t ofw_bus_get_node(device_t dev) { return dev; }
static int ofw_bus_node_status_okay(phandle_t node) { (void)node;return 1; }
static int OF_parent(phandle_t node) { return node==2 ? 3 : 99; }
static int OF_node_from_xref(phandle_t xref) { return xref==44 ? 2 : -1; }
static int tree_mode;
static int scenario, allocs;
static const char validpins[] = "emmc_cmd\0emmc_dat0\0emmc_dat1\0emmc_dat2\0emmc_dat3";
static int OF_hasprop(int node, const char *name) {
 (void)node;
 if (tree_mode && node==1 && strcmp(name,"pinctrl-0")==0) return 1;
 return strcmp(name,"pins")==0 || strcmp(name,"bias-pull-up")==0 ||
     (scenario==3 && strcmp(name,"bias-pull-down")==0) ||
     (scenario==4 && strcmp(name,"drive-strength")==0);
}
static int OF_getproplen(int node, const char *name) {
 (void)node; return strncmp(name,"bias-",5)==0 ? 0 : -1;
}
static int OF_nextprop(int node,const char *prev,char *prop,size_t len) {
 (void)node; assert(len>32);
 if (prev==NULL) { strcpy(prop,"pins"); return 1; }
 if (strcmp(prev,"pins")==0) { strcpy(prop,"bias-pull-up"); return 1; }
 if (scenario==4 && strcmp(prev,"bias-pull-up")==0) {
  strcpy(prop,"drive-strength"); return 1;
 }
 return 0;
}
static size_t strlcpy(char *dst,const char *src,size_t len) {
 size_t n=strlen(src); assert(n<len);memcpy(dst,src,n+1);return n;
}
static int OF_getprop(int node, const char *name, void *data, int len) {
 (void)node;(void)name;(void)data;(void)len; abort();
}
static int OF_child(int node) { return tree_mode && node==99 ? 1 : 0; }
static int OF_getencprop_alloc_multi(int node,const char *name,int size,void **out) {
 int *p=malloc(sizeof(int)); (void)node;(void)name;assert(size==4);assert(p);
 *p=44;*out=p;allocs++;return 1;
}
static int OF_peer(int node) { (void)node; return 0; }
static int OF_getprop_alloc(int node, const char *name, void **out) {
 const char *p = validpins; size_t len = sizeof(validpins);
 (void)node;(void)name;
 if (scenario==1) { p="emmc_cmd\0bad"; len=sizeof("emmc_cmd\0bad"); }
 if (scenario==2) { p="emmc_cmd"; len=strlen(p); }
 if (scenario==5) { p=""; len=1; }
 *out=malloc(len); assert(*out); memcpy(*out,p,len); allocs++; return len;
}
static void OF_prop_free(void *p) { free(p); allocs--; }
'''
PIN_TEST = r'''
static unsigned reads, writes;
static uint32_t regs[8];
static uint32_t read_reg(void *arg, uint32_t off) {
 (void)arg; assert(off%4==0 && off<sizeof(regs)); reads++; return regs[off/4];
}
static void write_reg(void *arg,uint32_t off,uint32_t value) {
 (void)arg; assert(off%4==0 && off<sizeof(regs)); writes++; regs[off/4]=value;
}
int main(void) {
 struct bcm2712_pinctrl_softc sc={0};
 struct bcm2712_pin_op ops[BCM2712_MAX_OPS];
 unsigned count=0, i; uint32_t old;
 assert(bcm2712_pin_parse(&sc,1,ops,&count,0)==0 && count==5 && allocs==0);
 for(i=0;i<8;i++) regs[i]=0xdeadbeef;
 old=regs[6]; bcm2712_pin_apply(ops,count,read_reg,write_reg,NULL);
 assert(reads==5 && writes==5);
 assert(regs[6]==((old & ~0x000ff0c0U)|0x000aa080U));
 for(i=0;i<8;i++) if(i!=6) assert(regs[i]==0xdeadbeef);
 for(scenario=1;scenario<=5;scenario++) {
  count=0; assert(bcm2712_pin_parse(&sc,1,ops,&count,0)!=0);
  assert(allocs==0 && reads==5 && writes==5);
 }
 assert(bcm2712_pin_parse(&sc,1,ops,&count,9)==E2BIG);
 count=0;
 assert(bcm2712_pin_plan(1,"aon_gpio5","sd_card_g",2,ops,&count,2)==0);
 assert(count==2 && ops[0].offset==12 && ops[0].mask==0x00f00000 &&
     ops[0].value==0x00400000 && ops[1].offset==20 &&
     ops[1].mask==0x30000000 && ops[1].value==0x20000000);
 for(i=2;i<=4;i++) {
  char name[12]; snprintf(name,sizeof(name),"aon_gpio%u",i); count=0;
  assert(bcm2712_pin_plan(1,name,"gpio",-1,ops,&count,2)==0);
  assert(count==1 && ops[0].offset==12 && ops[0].mask==(0xfU<<(i*4)) &&
      ops[0].value==0);
 }
 count=0;
 assert(bcm2712_pin_plan(0,"emmc_cmd","gpio",2,ops,&count,2)==EOPNOTSUPP);
 assert(count==0);
 assert(bcm2712_pin_plan(1,"aon_gpio6","gpio",2,ops,&count,2)==EINVAL);
 assert(bcm2712_pin_plan(1,"aon_gpio5","sd_card_g",2,ops,&count,1)==E2BIG);
 assert(count==0);
 scenario=0;tree_mode=1;device_sc=&sc;count=0;
 assert(bcm2712_pin_default_plan(3,99,ops,&count,0)==0 && count==5 && allocs==0);
 scenario=1;count=0;
 assert(bcm2712_pin_default_plan(3,99,ops,&count,0)==EINVAL && allocs==0);
 assert(reads==5 && writes==5);
 puts("PASS: production D0 pin planner/parser, exact pad fields, AON mux, preservation, malformed/unknown rejection before MMIO");
}
'''
REG = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
typedef int phandle_t;
struct gpioregulator_state { int val; uint32_t mask; };
struct gpioregulator_init_def { int nstates,npins,startup_delay_us,settling_time_us;
 struct gpioregulator_state *states; int **pins; int *enable_pin; int enable_pin_valid; };
struct regnode_std_param { bool enable_active_high; };
struct gpioregulator_reg_sc { struct gpioregulator_init_def *def; struct regnode_std_param *param; };
struct regnode { struct gpioregulator_reg_sc *sc; };
static void *regnode_get_softc(struct regnode *n) { return n->sc; }
static int writes, fail_pin=-1, values[2];
static int gpio_pin_set_active(int *pin, bool active) {
 writes++; if(*pin==fail_pin) return EIO; values[*pin]=active; return 0;
}
static int cell_len=-1; static uint32_t cell;
static int OF_getproplen(phandle_t n,const char *p) { (void)n;(void)p;return cell_len; }
static int OF_getencprop(phandle_t n,const char *p,void *v,int len) {
 (void)n;(void)p;assert(len==4);memcpy(v,&cell,4);return 4;
}
'''
REG_TEST = r'''
int main(void) {
 struct gpioregulator_state states[]={{1800000,1},{3300000,0}};
 int pin=0,*pins[]={&pin},delay;
 struct gpioregulator_init_def def={.nstates=2,.npins=1,.startup_delay_us=0,
     .settling_time_us=5000,.states=states,.pins=pins,.enable_pin=&pin};
 struct regnode_std_param param={true};
 struct gpioregulator_reg_sc sc={&def,&param}; struct regnode node={&sc};
 assert(gpioregulator_regnode_set_voltage(&node,3300000,3300000,&delay)==0);
 assert(writes==1 && values[0]==0 && delay==5000);
 def.startup_delay_us=7000;
 assert(gpioregulator_regnode_set_voltage(&node,1800000,1800000,&delay)==0);
 assert(writes==2 && values[0]==1 && delay==7000);
 def.settling_time_us=0;def.startup_delay_us=17;
 assert(gpioregulator_regnode_set_voltage(&node,3300000,3300000,&delay)==0 && delay==17);
 delay=-1;fail_pin=0;
 assert(gpioregulator_regnode_set_voltage(&node,1800000,1800000,&delay)==EIO && delay==0);
 delay=-1;
 assert(gpioregulator_regnode_enable(&node,true,&delay)==0 && delay==17);
 def.enable_pin_valid=1;delay=-1;
 assert(gpioregulator_regnode_enable(&node,true,&delay)==EIO && delay==0);
 fail_pin=-1;writes=0;
 assert(gpioregulator_regnode_set_voltage(&node,1200000,1200000,&delay)==EINVAL && writes==0 && delay==0);
 assert(gpioregulator_parse_delay(1,"x",&delay)==0 && delay==0);
 cell_len=4;cell=5000;
 assert(gpioregulator_parse_delay(1,"x",&delay)==0 && delay==5000);
 cell=INT_MAX;
 assert(gpioregulator_parse_delay(1,"x",&delay)==0 && delay==INT_MAX);
 cell=UINT32_MAX;
 assert(gpioregulator_parse_delay(1,"x",&delay)==EINVAL);
 for(cell_len=0;cell_len<=8;cell_len++) if(cell_len!=4)
  assert(gpioregulator_parse_delay(1,"x",&delay)==EINVAL);
 puts("PASS: production GPIO regulator callback returns >=5000us, preserves startup delay, rejects GPIO failure/no-state and malformed/overflow delay cells");
}
'''



def selected_dtb_program():
    """Retain exact FDT wire properties, including grouped UART and SDIO pins."""
    fixture=json.loads((ROOT/'tools/daemonarchy/tests/fixtures/pi500-pinctrl-defaults.json').read_text())
    assert fixture['dtb_sha256']=='400b2da08abb8fd0ffd65085d1e0e1e6c6d01cadd27385403ea3a3fd5a1537f4'
    paths=sorted(fixture['nodes'])
    ids={path:i+1 for i,path in enumerate(paths)}
    main_id=ids[fixture['main_provider']]
    aon_id=ids[fixture['aon_provider']]
    source=r"""
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bcm2712_pinctrl_policy.h"
typedef int phandle_t;
typedef int device_t;
#define BCM2712_MAX_OPS 128
struct bcm2712_pinctrl_softc { int aon; };
struct property { const char *name; const unsigned char *data; int len; };
struct node { int parent, nprops; struct property *props; };
"""
    node_init=['{0,0,NULL}']
    for path in paths:
        ident=ids[path]
        props=fixture['nodes'][path]['properties']
        for j,(_,raw) in enumerate(props.items()):
            b=bytes.fromhex(raw)
            data=','.join(str(v) for v in b) or '0'
            source+=f'static const unsigned char bytes_{ident}_{j}[]={{'+data+'};\n'
        if props:
            source+=f'static struct property props_{ident}[]={{\n'
            for j,(name,raw) in enumerate(props.items()):
                source+='{'+json.dumps(name)+f',bytes_{ident}_{j},{len(bytes.fromhex(raw))}'+'},\n'
            source+='};\n'
        parent=0 if path=='/' else ids[path.rsplit('/',1)[0] or '/']
        node_init.append('{'+f'{parent},{len(props)},'+(f'props_{ident}' if props else 'NULL')+'}')
    source+='static struct node nodes[]={'+','.join(node_init)+'};\n'
    source+=f'#define MAIN_PROVIDER {main_id}\n#define AON_PROVIDER {aon_id}\n'
    source+=f'#define SPI_GROUP {ids[fixture["main_provider"]+"/spi10_gpio2"]}\n'
    source+=r"""
static struct bcm2712_pinctrl_softc sc;
static int allocs;
static struct property *property(int node,const char *name) {
 int i;assert(node>0 && (unsigned)node<sizeof(nodes)/sizeof(*nodes));
 for(i=0;i<nodes[node].nprops;i++)
  if(strcmp(name,nodes[node].props[i].name)==0) return &nodes[node].props[i];
 return NULL;
}
static int OF_hasprop(int node,const char *name) { return property(node,name)!=NULL; }
static int OF_getproplen(int node,const char *name) {
 struct property *p=property(node,name);return p?p->len:-1;
}
static int OF_getprop(int node,const char *name,void *data,int len) {
 struct property *p=property(node,name);if(!p)return -1;
 assert(len>=p->len);memcpy(data,p->data,p->len);return p->len;
}
static int OF_getprop_alloc(int node,const char *name,void **out) {
 int len=OF_getproplen(node,name);if(len<=0)return len;
 *out=malloc(len);assert(*out);allocs++;
 return OF_getprop(node,name,*out,len);
}
static uint32_t cell(const unsigned char *p) {
 return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static int OF_getencprop_alloc_multi(int node,const char *name,int size,void **out) {
 struct property *p=property(node,name);int i,n;uint32_t *data;
 assert(size==4);if(!p)return -1;assert(p->len%size==0);n=p->len/size;
 if(!n)return 0;
 data=malloc(n*size);assert(data);allocs++;
 for(i=0;i<n;i++) data[i]=cell(p->data+i*size);
 *out=data;return n;
}
static void OF_prop_free(void *p) { free(p);allocs--; }
static int OF_nextprop(int node,const char *prev,char *data,size_t len) {
 int i=0;
 if(prev) {
  for(;i<nodes[node].nprops;i++) {
   if(strcmp(prev,nodes[node].props[i].name)==0)break;
  }
  i++;
 }
 if(i>=nodes[node].nprops)return 0;
 assert(strlen(nodes[node].props[i].name)<len);
 strcpy(data,nodes[node].props[i].name);return 1;
}
static int OF_parent(int node) { return node>0?nodes[node].parent:0; }
static int OF_child(int node) {
 unsigned i;for(i=1;i<sizeof(nodes)/sizeof(*nodes);i++)
  if(nodes[i].parent==node)return i;
 return 0;
}
static int OF_peer(int node) {
 unsigned i;for(i=node+1;i<sizeof(nodes)/sizeof(*nodes);i++)
  if(nodes[i].parent==nodes[node].parent)return i;
 return 0;
}
static int OF_node_from_xref(int xref) {
 unsigned i;struct property *p;
 for(i=1;i<sizeof(nodes)/sizeof(*nodes);i++) {
  p=property(i,"phandle");if(p && p->len==4 && cell(p->data)==(unsigned)xref)return i;
 }
 return -1;
}
static int ofw_bus_get_node(int dev) { return dev; }
static void *device_get_softc(int dev) {
 assert(dev==MAIN_PROVIDER || dev==AON_PROVIDER);return &sc;
}
static int ofw_bus_node_status_okay(int node) {
 struct property *p=property(node,"status");
 return !p || strcmp((const char *)p->data,"okay")==0 ||
     strcmp((const char *)p->data,"ok")==0;
}
static size_t strlcpy(char *dst,const char *src,size_t len) {
 size_t n=strlen(src);assert(n<len);memcpy(dst,src,n+1);return n;
}
"""
    pinpath='sys/arm64/broadcom/bcm2712_pinctrl.c'
    source+=function(pinpath,'bcm2712_pin_parse')
    source+=function(pinpath,'bcm2712_pin_default_plan')
    source+=r"""
static uint32_t regs[8];static unsigned reads,writes;
static uint32_t read_reg(void *arg,uint32_t off) {
 (void)arg;assert(off%4==0 && off<sizeof(regs));reads++;return regs[off/4];
}
static void write_reg(void *arg,uint32_t off,uint32_t value) {
 (void)arg;assert(off%4==0 && off<sizeof(regs));writes++;regs[off/4]=value;
}
int main(void) {
 struct bcm2712_pin_op ops[BCM2712_MAX_OPS];
 unsigned count=0,i;struct property *p;const unsigned char *saved;int savedlen;
 static const uint32_t masks[]={0x0000ffff,0x000f0000,0xffffffff,0x0000ffff,
     0x0003fc00,0x3f0ff030,0x000ff0ff,0};
 static const uint32_t values[]={0x00006670,0,0x11004444,0x00001111,
     0x00000800,0x28088020,0x000aa0aa,0};
 sc.aon=0;
 assert(bcm2712_pin_default_plan(MAIN_PROVIDER,1,ops,&count,0)==0);
 assert(count==37 && allocs==0 && reads==0 && writes==0);
 for(i=0;i<8;i++)regs[i]=0xdeadbeef;
 bcm2712_pin_apply(ops,count,read_reg,write_reg,NULL);
 for(i=0;i<8;i++)assert(regs[i]==((0xdeadbeef & ~masks[i])|values[i]));
 assert(reads==37 && writes==37);
 p=property(SPI_GROUP,"function");assert(p);saved=p->data;savedlen=p->len;
 p->data=(const unsigned char *)"unknown-function";p->len=17;count=0;
 assert(bcm2712_pin_default_plan(MAIN_PROVIDER,1,ops,&count,0)==EOPNOTSUPP);
 assert(allocs==0 && reads==37 && writes==37);
 p->data=saved;p->len=savedlen;
 sc.aon=1;count=0;
 assert(bcm2712_pin_default_plan(AON_PROVIDER,1,ops,&count,0)==0);
 assert(count==2 && allocs==0);
 /* Disabled AON I2C group is present but must not be applied; foreign RP1
  * USB defaults are present but belong to neither D0 controller. */
 assert(ops[0].offset==12 && ops[0].mask==0x00f00000 && ops[0].value==0x00400000);
 assert(ops[1].offset==20 && ops[1].mask==0x30000000 && ops[1].value==0x20000000);
 puts("PASS: exact enrolled Pi500 raw default groups: main37/AON2 operations, SPI/UART/SDIO/rails/button/SD, foreign-owner and disabled-node separation, unsupported function fails before MMIO");
}
"""
    return source

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc',default='cc')
    args=parser.parse_args()
    pinpath='sys/arm64/broadcom/bcm2712_pinctrl.c'
    regpath='sys/dev/gpio/gpioregulator.c'
    programs={
        'selected-dtb-defaults':selected_dtb_program(),
        'pinctrl':PIN+function(pinpath,'bcm2712_pin_parse')+
                   function(pinpath,'bcm2712_pin_default_plan')+PIN_TEST,
        'regulator':REG+function(regpath,'gpioregulator_regnode_enable')+
                    function(regpath,'gpioregulator_regnode_set_voltage')+
                    function(regpath,'gpioregulator_parse_delay')+REG_TEST,
    }
    with tempfile.TemporaryDirectory(prefix='pi5-sd-controls-') as tmp:
        for name,source in programs.items():
            path=pathlib.Path(tmp)/name
            path.with_suffix('.c').write_text(source)
            subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror',
                '-Wno-sign-compare','-I'+str(ROOT/'sys/arm64/broadcom'),
                str(path.with_suffix('.c')),'-o',str(path)],check=True)
            subprocess.run([str(path)],check=True)


if __name__=='__main__':
    main()
