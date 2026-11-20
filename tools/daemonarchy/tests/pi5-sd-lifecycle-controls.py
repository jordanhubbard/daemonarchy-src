#!/usr/bin/env python3
"""Exercise production BCM2712 SD power/resource cleanup with faulted providers.

No physical SD command, transfer, GPIO or electrical qualification is implied.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'sys/dev/sdhci/sdhci_fdt_bcm2712.c'


def function(source, name):
    match = re.search(r'(?m)^static (?:int|void)\n' + name + r'\(', source)
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
#include "sdhci_bcm2712_policy.h"
#undef NULL
#define NULL 0
#define SDHCI_SIGNAL_ENABLE 0x38
#define SDHCI_INT_ENABLE 0x34
#define SDHCI_SOFTWARE_RESET 0x2f
#define SDHCI_RESET_ALL 1
#define SYS_RES_MEMORY 1
#define SYS_RES_IRQ 2
#define GPIO_PIN_INPUT 1
struct regulator { unsigned held,enabled; int voltage; };
typedef struct regulator *regulator_t;
typedef int device_t;
typedef int phandle_t;
struct resource { int rid; };
struct gpio { bool held; };
typedef struct gpio *gpio_pin_t;
struct clock { bool held,enabled; };
typedef struct clock *clk_t;
#define power_off 0
#define power_on 1
#define vccq_330 1
#define vccq_180 2
struct mtx { int unused; };
struct sdhci_slot { bool locked; struct mtx mtx; struct {struct {int power_mode,vccq;} ios;} host; };
struct sdhci_fdt_softc {
 struct sdhci_slot slots[1];
 struct resource *irq_res,*mem_res[1];
 void *intrhand;
 unsigned num_slots;
};
'''

MOCKS = r'''
static struct bcm2712_sdhci_softc sc;
static struct regulator regs[2];
static struct clock clock;
static struct gpio cd,wp;
static struct resource host={0},cfg={1},irq={0};
static unsigned operation,fail_at,voltage_sets,reg_disabled,reg_released;
static unsigned freed_slots,irq_torn,pins_freed,clocks_disabled,clocks_released,resources_freed;
static unsigned writes,delays,forwarded;
static bool reset_stuck,signals_stuck,cfg_stuck,child_busy,voltage_failure_mutates;
static uint32_t cfg_ctrl,cfg_mode,signals;
static int fail_get,read_voltage,check_voltage,ios_error;
static int step(void) { return ++operation==fail_at ? EBUSY:0; }
static void *device_get_softc(device_t d) {assert(d==1);return &sc;}
static void *device_get_ivars(device_t d) {assert(d==2);return &sc.base.slots[0];}
static int sdhci_generic_update_ios(device_t d,device_t child) {assert(d==1&&child==2);return ios_error;}
static phandle_t ofw_bus_get_node(device_t d) {assert(d==1);return 42;}
static bool OF_hasprop(phandle_t n,const char *name) {
 assert(n==42);return !strcmp(name,"vmmc-supply")||!strcmp(name,"vqmmc-supply");
}
static int regulator_get_by_ofw_property(device_t d,phandle_t node,const char *name,regulator_t *out) {
 assert(d==1&&node==0);
 if(fail_get)return ENODEV;
 *out=&regs[!strcmp(name,"vqmmc-supply")];(*out)->held++;return 0;
}
static int regulator_get_voltage(regulator_t r,int *uv) {
 if(read_voltage)return EIO;*uv=r->voltage;return 0;
}
static int regulator_check_voltage(regulator_t r,int uv) {
 assert(r&&r->held&&uv==3300000);return check_voltage?ERANGE:0;
}
static int regulator_set_voltage(regulator_t r,int min,int max) {
 assert(r&&r->held&&min==max&&min>0);voltage_sets++;
 int e=step();if(!e||voltage_failure_mutates)r->voltage=min;return e;
}
static int regulator_enable(regulator_t r) {assert(r&&r->held);int e=step();if(!e)r->enabled++;return e;}
static int regulator_disable(regulator_t r) {
 assert(r&&r->held&&r->enabled);int e=step();if(!e){r->enabled--;reg_disabled++;}return e;
}
static int regulator_release(regulator_t r) {
 assert(r&&r->held&&!r->enabled);int e=step();if(!e){r->held--;reg_released++;}return e;
}
static void lock(struct sdhci_slot *s) {assert(s==&sc.base.slots[0]&&!s->locked&&sc.slot_initialized);s->locked=true;}
static void unlock(struct sdhci_slot *s) {assert(s->locked);s->locked=false;}
static void mtx_lock(struct mtx *m) {assert(m==&sc.base.slots[0].mtx);lock(&sc.base.slots[0]);}
#define SDHCI_LOCK(s) lock(s)
static void mtx_unlock(struct mtx *m) {assert(m==&sc.base.slots[0].mtx);unlock(&sc.base.slots[0]);}
#define SDHCI_UNLOCK(s) unlock(s)
static void host_write4(device_t d,struct sdhci_slot *s,unsigned off,uint32_t value) {
 assert(d==1&&s->locked&&sc.base.mem_res[0]==&host&&sc.clock_enabled);
 assert((off==SDHCI_SIGNAL_ENABLE||off==SDHCI_INT_ENABLE)&&value==0);writes++;
 if(off==SDHCI_SIGNAL_ENABLE)signals=value;
}
static uint32_t host_read4(device_t d,struct sdhci_slot *s,unsigned off) {
 assert(d==1&&s->locked&&off==SDHCI_SIGNAL_ENABLE);
 return signals_stuck?1:signals;
}
static void host_write1(device_t d,struct sdhci_slot *s,unsigned off,uint8_t value) {
 assert(d==1&&s->locked&&off==SDHCI_SOFTWARE_RESET&&value==SDHCI_RESET_ALL);writes++;
}
static uint8_t host_read1(device_t d,struct sdhci_slot *s,unsigned off) {
 assert(d==1&&s->locked&&off==SDHCI_SOFTWARE_RESET);return reset_stuck?1:0;
}
#define SDHCI_WRITE_4(d,s,o,v) host_write4(d,s,o,v)
#define SDHCI_READ_4(d,s,o) host_read4(d,s,o)
#define SDHCI_WRITE_1(d,s,o,v) host_write1(d,s,o,v)
#define SDHCI_READ_1(d,s,o) host_read1(d,s,o)
static void DELAY(unsigned us) {assert(us==10);delays++;}
static int bus_teardown_intr(device_t d,struct resource *r,void *cookie) {
 assert(d==1&&r==&irq&&cookie==&sc&&!sc.base.slots[0].locked);
 assert(sc.detaching&&!reset_stuck&&!signals_stuck);int e=step();if(!e)irq_torn++;return e;
}
static int sdhci_cleanup_slot(struct sdhci_slot *slot) {
 assert(slot==&sc.base.slots[0]&&!slot->locked&&!sc.base.intrhand);
 assert(sc.clock_enabled&&sc.cd_pin&&sc.wp_pin);int e=step();if(!e)freed_slots++;return e;
}
static void gpio_pin_release(gpio_pin_t pin) {
 assert(!sc.slot_initialized&&pin->held);pin->held=false;pins_freed++;
}
static void bus_write_4(struct resource *r,unsigned off,uint32_t value) {
 assert(r==&cfg&&!sc.slot_initialized&&sc.clock_enabled);writes++;
 if(off==BCM2712_CFG_CTRL)cfg_ctrl=value;
 else {assert(off==BCM2712_CFG_MAX_50MHZ);cfg_mode=value;}
}
static uint32_t bus_read_4(struct resource *r,unsigned off) {
 assert(r==&cfg&&sc.clock_enabled);if(cfg_stuck)return 0;
 return off==BCM2712_CFG_CTRL?cfg_ctrl:cfg_mode;
}
static int clk_disable(clk_t c) {
 assert(c==&clock&&c->enabled&&!regs[0].held&&!regs[1].held);
 int e=step();if(!e){c->enabled=false;clocks_disabled++;}return e;
}
static int clk_release(clk_t c) {
 assert(c==&clock&&c->held&&!c->enabled);int e=step();if(!e){c->held=false;clocks_released++;}return e;
}
static int bus_release_resource(device_t d,int type,int rid,struct resource *r) {
 assert(d==1&&!sc.clock&&!sc.slot_initialized&&!sc.base.intrhand);
 assert((type==SYS_RES_IRQ&&rid==0&&r==&irq)||
 (type==SYS_RES_MEMORY&&((rid==1&&r==&cfg)||(rid==0&&r==&host))));
 int e=step();if(!e)resources_freed++;return e;
}
static void sdhci_generic_intr(struct sdhci_slot *slot) {assert(slot==&sc.base.slots[0]);forwarded++;}
static int bus_generic_detach(device_t d) {assert(d==1);return child_busy?EBUSY:0;}
'''

MAIN = r'''
static void reset(void) {
 memset(&sc,0,sizeof(sc));memset(regs,0,sizeof(regs));memset(&clock,0,sizeof(clock));
 cd.held=wp.held=false;
 operation=fail_at=voltage_sets=reg_disabled=reg_released=0;
 freed_slots=irq_torn=pins_freed=clocks_disabled=clocks_released=resources_freed=0;
 writes=delays=forwarded=0;
 reset_stuck=signals_stuck=cfg_stuck=child_busy=voltage_failure_mutates=false;
 cfg_ctrl=cfg_mode=0;signals=1;fail_get=read_voltage=check_voltage=ios_error=0;
}
static void full_owner(void) {
 reset();
 sc.base.mem_res[0]=&host;sc.base.irq_res=&irq;sc.base.intrhand=&sc;sc.base.num_slots=1;
 sc.cfg=&cfg;sc.clock=&clock;sc.clock_enabled=sc.slot_initialized=sc.cfg_saved=true;
 clock.held=clock.enabled=true;
 sc.cd_pin=&cd;sc.wp_pin=&wp;cd.held=wp.held=true;
 sc.cfg_ctrl=0xc0001234;sc.cfg_mode=0x80000077;
 sc.vmmc=(struct bcm2712_supply){&regs[0],3300000,true,true};
 sc.vqmmc=(struct bcm2712_supply){&regs[1],1800000,true,true};
 for(unsigned i=0;i<2;i++){regs[i].held=regs[i].enabled=1;regs[i].voltage=3300000;}
}
int main(void) {
 reset();regs[0].voltage=1800000;
 assert(bcm2712_supply_get(1,"vmmc-supply",&sc.vmmc)==0);
 assert(sc.vmmc.voltage_set&&regs[0].voltage==3300000&&sc.vmmc.original_uv==1800000);
 assert(bcm2712_supply_power(&sc.vmmc,true)==0&&sc.vmmc.enabled&&regs[0].enabled==1);
 assert(bcm2712_supply_release(1,&sc.vmmc)==0&&!sc.vmmc.reg&&!regs[0].held);
 assert(regs[0].voltage==1800000&&reg_disabled==1);
 reset();fail_get=1;
 assert(bcm2712_supply_get(1,"vmmc-supply",&sc.vmmc)==ENODEV&&!sc.vmmc.reg);
 reset();regs[0].voltage=3300000;read_voltage=1;
 assert(bcm2712_supply_get(1,"vmmc-supply",&sc.vmmc)==EIO&&sc.vmmc.reg&&!sc.vmmc.voltage_set);
 assert(bcm2712_supply_release(1,&sc.vmmc)==0);
 reset();regs[0].voltage=3300000;check_voltage=1;
 assert(bcm2712_supply_get(1,"vmmc-supply",&sc.vmmc)==ERANGE&&!voltage_sets);
 assert(bcm2712_supply_release(1,&sc.vmmc)==0);
 reset();regs[0].voltage=1800000;fail_at=1;voltage_failure_mutates=true;
 assert(bcm2712_supply_get(1,"vmmc-supply",&sc.vmmc)==EBUSY&&sc.vmmc.voltage_set);
 assert(regs[0].voltage==3300000);fail_at=0;
 assert(bcm2712_supply_release(1,&sc.vmmc)==0&&regs[0].voltage==1800000);
 /* A failed I/O rail rollback cannot switch off the card rail underneath it. */
 full_owner();sc.vmmc.enabled=sc.vqmmc.enabled=false;regs[0].enabled=regs[1].enabled=0;
 sc.base.slots[0].host.ios.power_mode=power_on;sc.base.slots[0].host.ios.vccq=vccq_330;
 ios_error=EIO;fail_at=3;
 assert(bcm2712_sdhci_update_ios(1,2)==EBUSY&&sc.vmmc.enabled&&sc.vqmmc.enabled);
 assert(regs[0].enabled==1&&regs[1].enabled==1);
 ios_error=0;fail_at=0;sc.base.slots[0].host.ios.power_mode=power_off;
 assert(bcm2712_sdhci_update_ios(1,2)==0&&!sc.vmmc.enabled&&!sc.vqmmc.enabled);
 full_owner();sc.vmmc.enabled=sc.vqmmc.enabled=false;regs[0].enabled=regs[1].enabled=0;
 sc.base.slots[0].host.ios.power_mode=power_on;sc.base.slots[0].host.ios.vccq=vccq_330;
 ios_error=EIO;fail_at=4;
 assert(bcm2712_sdhci_update_ios(1,2)==EBUSY&&sc.vmmc.enabled&&!sc.vqmmc.enabled);
 ios_error=0;fail_at=0;sc.base.slots[0].host.ios.power_mode=power_off;
 assert(bcm2712_sdhci_update_ios(1,2)==0&&!sc.vmmc.enabled);
 full_owner();sc.vmmc.enabled=sc.vqmmc.enabled=false;regs[0].enabled=regs[1].enabled=0;
 sc.base.slots[0].host.ios.power_mode=power_on;sc.base.slots[0].host.ios.vccq=vccq_180;
 assert(bcm2712_sdhci_update_ios(1,2)==EOPNOTSUPP&&operation==0);
 full_owner();bcm2712_sdhci_intr(&sc);assert(forwarded==1);
 assert(bcm2712_sdhci_release(1)==0);
 assert(sc.detaching&&!sc.slot_initialized&&!sc.base.num_slots&&!sc.cfg_saved);
 assert(!sc.base.mem_res[0]&&!sc.cfg&&!sc.base.irq_res&&!sc.clock);
 assert(!sc.vmmc.reg&&!sc.vqmmc.reg&&!clock.held&&!regs[0].held&&!regs[1].held);
 assert(freed_slots==1&&irq_torn==1&&pins_freed==2&&reg_disabled==2&&reg_released==2);
 assert(clocks_disabled==1&&clocks_released==1&&resources_freed==3);
 unsigned total=operation;
 bcm2712_sdhci_intr(&sc);assert(forwarded==1);
 assert(bcm2712_sdhci_release(1)==0&&operation==total); /* Idempotent cleanup. */
 for(unsigned fail=1;fail<=total;fail++) {
  full_owner();fail_at=fail;
  assert(bcm2712_sdhci_release(1)==EBUSY&&operation==fail);
  assert(sc.base.mem_res[0]==&host);
  if(fail<=2)assert(sc.slot_initialized&&sc.clock_enabled&&sc.cd_pin&&sc.wp_pin);
  if(fail==3)assert(sc.vqmmc.enabled&&sc.vmmc.enabled&&regs[0].enabled==1);
  fail_at=0;assert(bcm2712_sdhci_release(1)==0);
  assert(!sc.clock&&!sc.vmmc.reg&&!sc.vqmmc.reg&&!sc.base.mem_res[0]);
  assert(freed_slots==1&&irq_torn==1&&pins_freed==2&&resources_freed==3);
 }
 full_owner();reset_stuck=true;
 assert(bcm2712_sdhci_release(1)==ETIMEDOUT&&delays==10000);
 assert(!freed_slots&&!irq_torn&&!pins_freed&&!reg_disabled&&!clocks_disabled);
 assert(sc.base.intrhand&&sc.slot_initialized&&sc.clock_enabled);
 reset_stuck=false;assert(bcm2712_sdhci_release(1)==0);
 full_owner();signals_stuck=true;
 assert(bcm2712_sdhci_release(1)==EIO&&!freed_slots&&!irq_torn);
 signals_stuck=false;assert(bcm2712_sdhci_release(1)==0);
 full_owner();cfg_stuck=true;
 assert(bcm2712_sdhci_release(1)==EIO&&sc.cfg_saved&&!sc.slot_initialized);
 assert(sc.vmmc.enabled&&sc.vqmmc.enabled&&sc.clock_enabled);
 cfg_stuck=false;assert(bcm2712_sdhci_release(1)==0);
 full_owner();child_busy=true;
 assert(bcm2712_sdhci_detach(1)==EBUSY&&!sc.detaching&&!writes);
 child_busy=false;assert(bcm2712_sdhci_detach(1)==0);
 puts("PASS: BCM2712 SD shutdown/reset bounds, supply rollback, owner retention and cleanup retries");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = SOURCE.read_text()
    structs = source[source.index('struct bcm2712_supply {'):source.index('static int\nbcm2712_supply_get')]
    callbacks = '\n'.join(function(source, name) for name in (
        'bcm2712_supply_get', 'bcm2712_supply_power', 'bcm2712_supply_release',
        'bcm2712_sdhci_quiesce', 'bcm2712_sdhci_release',
        'bcm2712_sdhci_intr', 'bcm2712_sdhci_detach',
        'bcm2712_supply_rollback', 'bcm2712_sdhci_update_ios'))
    with tempfile.TemporaryDirectory(prefix='pi5-sd-controls-') as tmp:
        path = Path(tmp)
        (path / 'controls.c').write_text(PREFIX + structs + MOCKS + callbacks + MAIN)
        subprocess.run([args.cc, '-std=gnu17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-misleading-indentation',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(SOURCE.parent), str(path / 'controls.c'),
                        '-o', str(path / 'controls')], check=True)
        subprocess.run([str(path / 'controls')], check=True)


if __name__ == '__main__':
    main()
