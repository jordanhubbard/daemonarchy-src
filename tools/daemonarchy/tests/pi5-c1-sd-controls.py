#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Run production C0/C1 pin plans and callbacks against exact firmware groups.

Host register models do not establish electrical operation or physical boot.
"""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('providers', HERE / 'pi5-sd-provider-controls.py')
providers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(providers)

DEFAULTS = r'''
static uint32_t regs[12];static unsigned reads,writes;
static uint32_t read_reg(void *arg,uint32_t off) {
 (void)arg;assert(off%4==0 && off<sizeof(regs));reads++;return regs[off/4];
}
static void write_reg(void *arg,uint32_t off,uint32_t value) {
 (void)arg;assert(off%4==0 && off<sizeof(regs));writes++;regs[off/4]=value;
}
int main(void) {
 struct bcm2712_pin_op ops[BCM2712_MAX_OPS];
 /* Independent register expectations for the exact C1 firmware defaults.
  * Main GPIO muxes 1/2/3/4,20,24..35; SD pulls are register 11.
  * DAT2 is bits 13:12, distinct from DAT4 bits 17:16. */
 static const uint32_t masks[12]={0x000ffff0,0,0x000f0000,0xffffffff,
   0x0000ffff,0,0,0x00ff0000,0x03000000,0x03ffc3fc,0,0x0000ff0c};
 static const uint32_t values[12]={0x00065600,0,0,0x44004443,
   0x00003434,0,0,0x00020000,0x02000000,0x02aa0220,0,0x0000aa08};
 unsigned count=0,i;struct property *p;const unsigned char *saved;int len;
 sc.profile=2;
 assert(bcm2712_pin_default_plan(MAIN_PROVIDER,1,ops,&count,0)==0);
 assert(count==37 && !allocs && !reads && !writes);
 for(i=0;i<12;i++)regs[i]=0xdeadbeef;
 bcm2712_pin_apply(ops,count,read_reg,write_reg,NULL);
 for(i=0;i<12;i++)assert(regs[i]==((0xdeadbeef & ~masks[i])|values[i]));
 assert(reads==37 && writes==37);
 p=property(SPI_GROUP,"function");assert(p);saved=p->data;len=p->len;
 p->data=(const unsigned char *)"unknown-function";p->len=17;count=0;
 assert(bcm2712_pin_default_plan(MAIN_PROVIDER,1,ops,&count,0)==EOPNOTSUPP);
 assert(!allocs && reads==37 && writes==37);p->data=saved;p->len=len;
 sc.profile=3;count=0;
 assert(bcm2712_pin_default_plan(AON_PROVIDER,1,ops,&count,0)==0);
 assert(count==2 && !allocs);
 assert(ops[0].offset==12 && ops[0].mask==0x00f00000 && ops[0].value==0x00600000);
 assert(ops[1].offset==28 && ops[1].mask==3 && ops[1].value==2);
 count=0;assert(bcm2712_pin_plan(4,"gpio20","gpio",2,ops,&count,128)==EINVAL);
 assert(count==0 && reads==37 && writes==37);
 puts("PASS: exact C1 firmware main37/AON2 plans, independent mux/pull fields, invalid profile/function before MMIO");
}
'''

FLAGS = r'''
int main(void) {
 sc.profile=3;regs[3]=0x00600000;regs[7]=0xdeadbeef;
 assert(bcm2712_pinctrl_set_flags(1,2,5,GPIO_PIN_INPUT|GPIO_PIN_PULLUP)==0);
 assert(regs[3]==0x00600000 && regs[7]==0xdeadbeee);
 drop_write=1;regs[7]=0;
 assert(bcm2712_pinctrl_set_flags(1,2,5,GPIO_PIN_INPUT|GPIO_PIN_PULLUP)==EIO);
 drop_write=0;regs[3]=0x00400000;writes=0;
 assert(bcm2712_pinctrl_set_flags(1,2,5,GPIO_PIN_INPUT)==EINVAL && !writes);
 assert(bcm2712_pinctrl_set_flags(1,2,5,GPIO_PIN_OUTPUT)==EOPNOTSUPP);
 for(unsigned pin=2;pin<=4;pin++) {
  regs[3]=0xdeadbeef;regs[6]=0;writes=0;
  assert(bcm2712_pinctrl_set_flags(1,2,pin,GPIO_PIN_OUTPUT|GPIO_PIN_PULLUP)==0);
  assert((regs[3] & (15U<<(pin*4)))==0);
  assert(regs[6]==(2U<<((pin+10)*2)) && writes==2 && !sc.lock);
 }
 /* A C0/C1 GPIO width cannot be paired with the D0 provider. */
 sc.profile=1;writes=0;
 assert(bcm2712_pinctrl_set_flags(1,2,2,GPIO_PIN_OUTPUT)==EINVAL && !writes);
 puts("PASS: C1 AON mux6/card-detect pulls, rail/reset GPIO pads, dropped writes and mismatched bank profile");
}
'''

ATTACH = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bcm2712_pinctrl_policy.h"
typedef int device_t;
typedef int phandle_t;
struct resource { unsigned size; };
struct bcm2712_pinctrl_softc { struct resource *mem;int lock,profile; };
static struct bcm2712_pinctrl_softc sc;
static struct resource mem;
static const char *compatible;
static int enabled=1,allocated,published,writes,plan_error,alloc_error;
#define SYS_RES_MEMORY 1
#define RF_ACTIVE 1
#define BUS_PROBE_DEFAULT 0
#define MTX_SPIN 1
#define BCM2712_MAX_OPS 128
static int ofw_bus_status_okay(int d) { (void)d;return enabled; }
static int ofw_bus_is_compatible(int d,const char *s) { (void)d;return !strcmp(s,compatible); }
static void device_set_desc(int d,const char *s) { (void)d;(void)s; }
static void device_printf(int d,const char *s,int error) { (void)d;(void)s;(void)error; }
static void *device_get_softc(int d) { (void)d;return &sc; }
static struct resource *bus_alloc_resource_any(int d,int t,int *rid,int flags) {
 (void)d;assert(t==1 && *rid==0 && flags==1);if(alloc_error)return NULL;
 assert(!allocated);allocated=1;return &mem;
}
static int bus_release_resource(int d,int t,int rid,struct resource *r) {
 (void)d;assert(t==1 && !rid && r==&mem && allocated);allocated=0;return 0;
}
static unsigned rman_get_size(struct resource *r) { return r->size; }
static int OF_peer(int n) { assert(!n);return 1; }
static int bcm2712_pin_default_plan(int d,int n,struct bcm2712_pin_op *ops,
 unsigned *count,unsigned depth) {
 (void)d;(void)ops;assert(n==1 && !*count && !depth);return plan_error;
}
static void mtx_init(int *m,const char *s,void *v,int t) {
 (void)s;(void)v;assert(t==1);*m=0;
}
static void mtx_lock_spin(int *m) { assert(!*m);*m=1; }
static void mtx_unlock_spin(int *m) { assert(*m);*m=0; }
static uint32_t bcm2712_pin_read(void *arg,uint32_t off) {
 (void)arg;(void)off;assert(0);return 0;
}
static void bcm2712_pin_write(void *arg,uint32_t off,uint32_t value) {
 (void)arg;(void)off;(void)value;writes++;
}
static void fdt_pinctrl_register(int d,void *p) { (void)d;assert(!p && allocated);published++; }
'''

ATTACH_TEST = r'''
int main(void) {
 static const char *names[]={"brcm,bcm2712d0-pinctrl","brcm,bcm2712d0-aon-pinctrl",
   "brcm,bcm2712c0-pinctrl","brcm,bcm2712c0-aon-pinctrl"};
 static const unsigned sizes[]={0x20,0x1c,0x30,0x20};
 compatible="unknown";assert(bcm2712_pinctrl_probe(1)==ENXIO);
 for(unsigned i=0;i<4;i++) {
  compatible=names[i];enabled=0;assert(bcm2712_pinctrl_probe(1)==ENXIO);
  enabled=1;assert(!bcm2712_pinctrl_probe(1));
  for(unsigned failure=0;failure<4;failure++) {
   allocated=published=writes=0;mem.size=sizes[i];plan_error=alloc_error=0;
   memset(&sc,0,sizeof(sc));
   if(failure==0)alloc_error=1;
   if(failure==1)mem.size--;
   if(failure==2)plan_error=EOPNOTSUPP;
   int error=bcm2712_pinctrl_attach(1);assert(sc.profile==(int)i && !writes);
   if(failure<3)assert(error && !allocated && !published);
   else assert(!error && allocated && published==1);
  }
 }
 puts("PASS: actual four-profile probe/attach selection, MMIO bounds, failed preflight keeps providers unpublished");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    prefix = providers.PIN_FLAGS.replace('((pcell_t *)out)[0]=15;', '((pcell_t *)out)[0]=17;')
    programs = {
        'defaults': providers.selected_dtb_program('pi5-c1-pinctrl-defaults.json', DEFAULTS),
        'flags': prefix + providers.function('sys/arm64/broadcom/bcm2712_pinctrl.c',
                                            'bcm2712_pinctrl_set_flags') + FLAGS,
        'attach': ATTACH + providers.function('sys/arm64/broadcom/bcm2712_pinctrl.c',
                                             'bcm2712_pinctrl_probe') +
                  providers.function('sys/arm64/broadcom/bcm2712_pinctrl.c',
                                     'bcm2712_pinctrl_attach') + ATTACH_TEST,
    }
    with tempfile.TemporaryDirectory(prefix='pi5-c1-sd-') as temp:
        for name, code in programs.items():
            src = Path(temp) / (name + '.c')
            binary = Path(temp) / name
            src.write_text(code)
            cmd = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-Wno-sign-compare', '-Wno-unused-function', '-fsanitize=address,undefined',
                   '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                   '-I' + str(providers.ROOT / 'sys/arm64/broadcom'), str(src), '-o', str(binary)]
            if 'clang' in Path(args.cc).name:
                probe = subprocess.run([args.cc, '-Werror', '-Wno-gcc-install-dir-libstdcxx',
                                        '-x', 'c', '-fsyntax-only', '-'], input='', text=True,
                                       capture_output=True)
                if probe.returncode == 0:
                    cmd.insert(1, '-Wno-gcc-install-dir-libstdcxx')
            subprocess.run(cmd, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
