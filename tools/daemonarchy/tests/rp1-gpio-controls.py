#!/usr/bin/env python3
"""Production RP1 GPIO callbacks with register and ownership fault controls.

Native compilation and physical PHY reset remain independent checks.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'sys/arm64/broadcom/rp1_gpio.c'


def function(source, name):
    match = re.search(r'(?m)^static (?:int|void|uint32_t)\n' + name + r'\(', source)
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define GPIO_PIN_INPUT 1U
#define GPIO_PIN_OUTPUT 2U
#define GPIO_PIN_PULLUP 0x20U
#define GPIO_PIN_PULLDOWN 0x40U
#define GPIO_PIN_PRESET_LOW 0x400U
#define GPIO_PIN_PRESET_HIGH 0x800U
#undef NULL
#define NULL 0
#define MTX_DEF 0
#define SYS_RES_MEMORY 1
#define RF_ACTIVE 1
#define ENXIO 6
#define EOPNOTSUPP 95
struct mtx { int held; };
struct resource { unsigned index; uint64_t size; };
typedef int device_t;
typedef int phandle_t;
typedef uint32_t pcell_t;
struct rp1_gpio_softc {
 device_t busdev;
 struct resource *mem[3];
 struct mtx lock;
 bool lock_initialized, ready, published;
};
static struct rp1_gpio_softc sc;
static struct resource mem[3] = {{0,RP1_GPIO_SIZE},{1,RP1_GPIO_SIZE},{2,RP1_GPIO_SIZE}};
static uint32_t regs[3][RP1_GPIO_SIZE/4];
static struct { unsigned region,offset; uint32_t value; } writes[32];
static unsigned nwrites, reads, allocations, releases, release_calls, fail_release;
static unsigned alloc_fail;
static bool child_fail, attach_fail, busy_child;
static bool child_exists, provider;
static unsigned registrations, destroyed, quarantined;
static uint64_t rman_get_size(struct resource *r) { return r->size; }
static void *device_get_softc(device_t dev) { assert(dev == 1); return &sc; }
static void mtx_lock(struct mtx *m) { assert(sc.lock_initialized && !m->held); m->held=1; }
static void mtx_unlock(struct mtx *m) { assert(m->held); m->held=0; }
static void mtx_init(struct mtx *m,const char *name,void *arg,int flags) {
 assert(m==&sc.lock && name && !arg && !flags); m->held=0;
}
static void mtx_destroy(struct mtx *m) { assert(m==&sc.lock && !m->held); destroyed++; }
static uint32_t bus_read_4(struct resource *r,unsigned offset) {
 assert(offset%4==0 && offset<RP1_GPIO_SIZE && sc.lock.held);
 reads++; return regs[r->index][offset/4];
}
static void bus_write_4(struct resource *r,unsigned offset,uint32_t value) {
 unsigned bank=offset/0x4000, alias=offset%0x4000;
 unsigned rw=bank*0x4000+(alias&0xfff);
 assert(offset%4==0 && offset<RP1_GPIO_SIZE && sc.lock.held && nwrites<32);
 writes[nwrites].region=r->index; writes[nwrites].offset=offset;
 writes[nwrites++].value=value;
 if (alias>=RP1_GPIO_CLR) regs[r->index][rw/4]&=~value;
 else if (alias>=RP1_GPIO_SET) regs[r->index][rw/4]|=value;
 else if (alias>=RP1_GPIO_XOR) regs[r->index][rw/4]^=value;
 else regs[r->index][rw/4]=value;
}
static phandle_t ofw_bus_get_node(device_t dev) { assert(dev==1); return 42; }
static int OF_getproplen(phandle_t node,const char *name) {
 assert(node==42 && !strcmp(name,"gpio-controller")); return 0;
}
static int OF_getencprop(phandle_t node,const char *name,void *out,size_t len) {
 assert(node==42 && !strcmp(name,"#gpio-cells") && len==4); *(pcell_t *)out=2; return 4;
}
static const char *device_get_nameunit(device_t dev) { assert(dev==1); return "rp1_gpio0"; }
static struct resource *bus_alloc_resource_any(device_t dev,int type,int *rid,int flags) {
 assert(dev==1 && type==SYS_RES_MEMORY && flags==RF_ACTIVE && *rid>=0 && *rid<3);
 allocations++; return allocations==alloc_fail ? NULL : &mem[*rid];
}
static int bus_release_resource(device_t dev,int type,int rid,struct resource *r) {
 assert(dev==1 && type==SYS_RES_MEMORY && rid>=0 && rid<3 && r==&mem[rid]);
 if (++release_calls==fail_release) return EBUSY;
 releases++; return 0;
}
static device_t gpiobus_add_bus_deferred(device_t dev) {
 assert(dev==1 && !child_exists); if(child_fail) return 0; child_exists=true; return 2;
}
static void bus_attach_children(device_t dev) { assert(dev==1 && child_exists); }
static bool device_is_attached(device_t dev) { assert(dev==2); return !attach_fail; }
static bool gpiobus_ready(device_t dev) { assert(dev==2); return !attach_fail; }
static int device_delete_child(device_t dev,device_t child) {
 assert(dev==1 && child==2 && child_exists);
 if (busy_child) return EBUSY;
 child_exists=false; return 0;
}
static void ofw_gpiobus_register_provider(device_t dev) {
 assert(dev==1 && sc.ready && child_exists && !attach_fail); provider=true; registrations++;
}
static void gpiobus_quarantine(device_t dev,int error) {
 assert(dev==2 && error!=0); quarantined++;
}
static int device_printf(device_t dev,const char *fmt,...) {
 assert(dev==1 && fmt); return 0;
}
'''

MAIN = r'''
static void reset(void) {
 memset(&sc,0,sizeof(sc)); memset(regs,0,sizeof(regs));
 sc.lock_initialized=true;
 for(unsigned i=0;i<3;i++) { sc.mem[i]=&mem[i]; mem[i].size=RP1_GPIO_SIZE; }
 nwrites=reads=allocations=releases=release_calls=fail_release=alloc_fail=0;
 child_fail=attach_fail=busy_child=child_exists=provider=false;
 registrations=destroyed=quarantined=0;
}
static void write_at(unsigned i,unsigned region,unsigned off,uint32_t value) {
 assert(i<nwrites && writes[i].region==region && writes[i].offset==off);
 assert(writes[i].value==value);
}
int main(void) {
 unsigned bank, local, value;
 uint32_t flags,pin,cells[2]={32,1};
 reset();
 for(unsigned p=0;p<RP1_GPIO_PINS;p++) {
  assert(rp1_gpio_location(p,&bank,&local)==0);
  assert(bank==(p<28?0U:p<34?1U:2U));
  assert(local==p-(bank==0?0U:bank==1?28U:34U));
  unsigned b=bank*0x4000, c=(b+RP1_GPIO_CTRL(local))/4;
  unsigned pad=(b+RP1_PAD(local))/4, oe=(b+RP1_RIO_OE)/4;
  unsigned out=(b+RP1_RIO_OUT)/4, input=(b+RP1_RIO_IN)/4;
  uint32_t mask=1U<<local;
  regs[0][c]=0x100000|RP1_GPIO_OEOVER|RP1_GPIO_OUTOVER|RP1_GPIO_INOVER|3;
  regs[2][pad]=0x1000|RP1_PAD_OD|RP1_PAD_DOWN;
  regs[1][oe]=0xffffffff; regs[1][out]=~mask; nwrites=0;
  assert(rp1_gpio_setflags(1,p,GPIO_PIN_OUTPUT|GPIO_PIN_PRESET_HIGH|GPIO_PIN_PULLUP)==0);
  assert(nwrites==6);
  write_at(0,0,b+RP1_GPIO_CTRL(local),0x100000|RP1_GPIO_FUNC|RP1_GPIO_OE_DISABLE);
  write_at(1,1,b+RP1_GPIO_CLR+RP1_RIO_OE,mask);
  write_at(2,1,b+RP1_GPIO_SET+RP1_RIO_OUT,mask);
  write_at(3,2,b+RP1_PAD(local),0x1000|RP1_PAD_IE|RP1_PAD_UP);
  write_at(4,0,b+RP1_GPIO_CTRL(local),0x100000|RP1_GPIO_FUNC);
  write_at(5,1,b+RP1_GPIO_SET+RP1_RIO_OE,mask);
  assert(regs[1][oe]==0xffffffff && regs[1][out]==0xffffffff);
  assert(rp1_gpio_getflags(1,p,&flags)==0 && flags==(GPIO_PIN_OUTPUT|GPIO_PIN_PULLUP));
  nwrites=0;
  assert(rp1_gpio_setflags(1,p,GPIO_PIN_OUTPUT|GPIO_PIN_PRESET_LOW)==0);
  assert((regs[1][out]&mask)==0 && (regs[1][oe]&mask)!=0);
  assert(rp1_gpio_set(1,p,1)==0 && (regs[1][out]&mask)!=0);
  assert(rp1_gpio_toggle(1,p)==0 && (regs[1][out]&mask)==0);
  regs[1][input]=mask;
  assert(rp1_gpio_get(1,p,&value)==0 && value==1);
  regs[1][input]=0;
  assert(rp1_gpio_get(1,p,&value)==0 && value==0);
  nwrites=0;
  assert(rp1_gpio_setflags(1,p,GPIO_PIN_INPUT|GPIO_PIN_PULLDOWN)==0);
  assert(nwrites==4 && regs[1][oe]==(0xffffffff&~mask));
  assert(rp1_gpio_getflags(1,p,&flags)==0 && flags==(GPIO_PIN_INPUT|GPIO_PIN_PULLDOWN));
  regs[0][c]=3;
  assert(rp1_gpio_getflags(1,p,&flags)==0 && flags==0);
 }
 reset();
 assert(rp1_gpio_location(54,&bank,&local)==EINVAL);
 assert(rp1_gpio_location(UINT32_MAX,&bank,&local)==EINVAL);
 const unsigned invalid[]={0,3,GPIO_PIN_INPUT|RP1_GPIO_PRESET,
  GPIO_PIN_OUTPUT|RP1_GPIO_PRESET,GPIO_PIN_OUTPUT|GPIO_PIN_PULLUP|GPIO_PIN_PULLDOWN,
  GPIO_PIN_INPUT|GPIO_PIN_PRESET_LOW};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)
  assert(rp1_gpio_setflags(1,32,invalid[i])==EINVAL);
 assert(rp1_gpio_setflags(1,32,GPIO_PIN_OUTPUT|4)==EOPNOTSUPP);
 assert(rp1_gpio_setflags(1,54,GPIO_PIN_OUTPUT)==EINVAL);
 assert(rp1_gpio_set(1,54,0)==EINVAL && rp1_gpio_set(1,32,2)==EINVAL);
 assert(nwrites==0 && reads==0);
 assert(rp1_gpio_map(1,17,42,2,cells,&pin,&flags)==ENXIO);
 sc.ready=true;
 assert(rp1_gpio_map(1,17,42,2,cells,&pin,&flags)==0 && pin==32 && flags==1);
 cells[1]=0; assert(rp1_gpio_map(1,17,42,2,cells,&pin,&flags)==0 && flags==0);
 cells[1]=2; assert(rp1_gpio_map(1,17,42,2,cells,&pin,&flags)==EOPNOTSUPP);
 cells[0]=54; assert(rp1_gpio_map(1,17,42,2,cells,&pin,&flags)==EINVAL);
 cells[0]=32;
 assert(rp1_gpio_map(1,17,43,2,cells,&pin,&flags)==EINVAL);
 assert(rp1_gpio_map(1,17,42,1,cells,&pin,&flags)==EINVAL);
 for(unsigned fail=1;fail<=3;fail++) {
  reset(); memset(&sc,0,sizeof(sc)); alloc_fail=fail;
  assert(rp1_gpio_attach(1)==ENXIO && allocations==fail && releases==fail-1);
  assert(!provider && !sc.busdev && !sc.mem[0] && !sc.lock_initialized);
  assert(nwrites==0 && reads==0);
 }
 for(unsigned i=0;i<3;i++) {
  reset(); memset(&sc,0,sizeof(sc)); mem[i].size=RP1_GPIO_SIZE-1;
  assert(rp1_gpio_attach(1)==ENXIO && allocations==i+1 && releases==i+1);
  assert(!provider && !sc.lock_initialized && nwrites==0 && reads==0);
 }
 reset(); memset(&sc,0,sizeof(sc)); child_fail=true;
 assert(rp1_gpio_attach(1)==ENOMEM && destroyed==1 && releases==3 && !provider);
 reset(); memset(&sc,0,sizeof(sc)); attach_fail=true;
 assert(rp1_gpio_attach(1)==ENXIO && !child_exists && destroyed==1 && !provider);
 reset(); memset(&sc,0,sizeof(sc)); attach_fail=busy_child=true;
 assert(rp1_gpio_attach(1)==0 && quarantined==1 && !provider && !sc.ready);
 assert(sc.mem[0] && sc.mem[1] && sc.mem[2] && sc.lock_initialized);
 busy_child=false; assert(rp1_gpio_release(1)==0 && !sc.mem[0]);
 for(unsigned fail=1;fail<=3;fail++) {
  reset(); fail_release=fail;
  assert(rp1_gpio_release(1)==EBUSY && sc.lock_initialized && destroyed==0);
  assert(sc.mem[3-fail] && releases==fail-1);
  fail_release=0;
  assert(rp1_gpio_release(1)==0 && !sc.lock_initialized && destroyed==1);
 }
 reset(); memset(&sc,0,sizeof(sc));
 assert(rp1_gpio_attach(1)==0 && provider && registrations==1 && sc.ready && sc.published);
 assert(nwrites==0 && reads==0);
 assert(rp1_gpio_release(1)==EBUSY && releases==0 && destroyed==0);
 puts("PASS: RP1 GPIO 54 pins, bank boundaries, preset ordering, active-low mapping and attach/cleanup faults");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = SOURCE.read_text()
    constants = '\n'.join(re.findall(r'(?m)^#define RP1_.*$', source))
    functions = '\n'.join(function(source, name) for name in (
        'rp1_gpio_location', 'rp1_gpio_read', 'rp1_gpio_write',
        'rp1_gpio_getflags', 'rp1_gpio_setflags', 'rp1_gpio_get',
        'rp1_gpio_output', 'rp1_gpio_set', 'rp1_gpio_toggle', 'rp1_gpio_map',
        'rp1_gpio_release', 'rp1_gpio_attach'))
    with tempfile.TemporaryDirectory(prefix='rp1-gpio-controls-') as tmp:
        path = Path(tmp)
        (path / 'controls.c').write_text(constants + '\n' + PREFIX + functions + MAIN)
        subprocess.run([args.cc, '-std=gnu17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', str(path / 'controls.c'),
                        '-o', str(path / 'controls')], check=True)
        subprocess.run([str(path / 'controls')], check=True)


if __name__ == '__main__':
    main()
