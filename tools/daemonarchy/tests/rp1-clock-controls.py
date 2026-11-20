#!/usr/bin/env python3
"""Compile production RP1 clock math/callbacks against a register/clock model.

Checks do not establish physical PLL lock, clock timing or Ethernet behavior.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'sys/arm64/broadcom/rp1_clock.c'


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
#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#define CLK_SET_ROUND_UP 1
#define CLK_SET_ROUND_DOWN 2
#define CLK_SET_ROUND_ANY 3
#define CLK_SET_DRYRUN 0x10000
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
typedef int device_t;
typedef uint32_t phandle_t;
typedef uint32_t pcell_t;
typedef void *clk_t;
struct mtx { bool held; };
struct resource { int unused; };
struct clkdom { int unused; };
struct clknode;
'''

MOCKS = r'''
struct clknode {
 struct rp1_clknode_softc sc;
 int parent;
 uint64_t frequency;
 unsigned recalcs;
};
static struct rp1_clock_softc sc;
static struct clknode nodes[RP1_CLOCK_COUNT], xosc;
static struct resource mem;
static struct clkdom domain;
static uint32_t regs[0x10038/4];
static unsigned writes, reads, delays, reparents, pending_feedback;
static bool no_lock, no_select, reject_write;
static unsigned fail_reg = UINT32_MAX;
static const unsigned assigned_ids[] = {0,1,3,9,16,4,10,12,6,13,31,32,29};
static const unsigned assigned_rates[] = {1000000000,1536000000,200000000,125000000,
 125000000,61440000,153600000,200000000,100000000,50000000,1000000,200000000,50000000};
static uint32_t property_clocks[26], property_rates[13];
static int clock_len=sizeof(property_clocks),rate_len=sizeof(property_rates);
static bool assigned_parents;
static void *device_get_softc(device_t d) { assert(d==1);return &sc; }
static void *clknode_get_softc(struct clknode *c) { return &c->sc; }
static void clknode_init_parent_idx(struct clknode *c,int i) { c->parent=i; }
static int clknode_get_parent_idx(struct clknode *c) { return c->parent; }
static const char **clknode_get_parent_names(struct clknode *c) {
 return sc.parents[c-nodes];
}
static struct clknode *clknode_find_by_name(const char *name) {
 if (!strcmp(name,"xosc"))return &xosc;
 for(unsigned i=0;i<RP1_CLOCK_COUNT;i++)if(!strcmp(name,sc.names[i]))return &nodes[i];
 return NULL;
}
static struct clknode *clknode_find_by_id(struct clkdom *d,intptr_t id) {
 assert(d==&domain);
 for(unsigned i=0;i<RP1_CLOCK_COUNT;i++)if(rp1_clocks[i].id==id)return &nodes[i];
 return NULL;
}
static int clknode_get_freq(struct clknode *,uint64_t *);
static int rp1_clknode_mux(struct clknode *,int);
static int rp1_clknode_recalc(struct clknode *,uint64_t *);
static int clknode_set_parent_by_idx(struct clknode *c,int i) {
 assert(i>=0 && (unsigned)i<c->sc.def->nparents);
 int e=rp1_clknode_mux(c,i);if(e)return e;
 reparents++;c->parent=i;c->frequency=0;return 0;
}
static void mtx_lock(struct mtx *m) { assert(m==&sc.lock && !m->held);m->held=true; }
static void mtx_unlock(struct mtx *m) { assert(m==&sc.lock && m->held);m->held=false; }
static uint32_t bus_read_4(struct resource *r,unsigned reg) {
 assert(r==&mem && reg%4==0 && reg<sizeof(regs) && pending_feedback==0);reads++;
 return regs[reg/4];
}
static void bus_write_4(struct resource *r,unsigned reg,uint32_t value) {
 assert(r==&mem && sc.lock.held && reg%4==0 && reg<sizeof(regs));writes++;
 if(reject_write && reg==fail_reg)return;
 regs[reg/4]=value;
 if(reg==0x8008 || reg==0xc008)pending_feedback=value==0?1:4;
 else if((reg==0x800c || reg==0xc00c) && pending_feedback)
  pending_feedback=pending_feedback==4?0:2;
 else if((reg==0x8004 || reg==0xc004) && pending_feedback)pending_feedback=3;
 if((reg==0x8000 || reg==0xc000) && (value&1)!=0) {
  if(!no_lock)regs[reg/4]|=RP1_PLL_LOCK;
  else regs[reg/4]&=~RP1_PLL_LOCK;
 }
 for(unsigned i=0;i<RP1_CLOCK_COUNT;i++) {
  const struct rp1_clock_def *d=&rp1_clocks[i];
  if(d->kind!=RP1_LEAF || d->reg!=reg)continue;
  if(no_select)continue;
  regs[d->selected/4]=((value&RP1_CLOCK_ENABLE)!=0 || d->id==12 || d->id==13)?
   1U<<(value&(d->id==12?3U:1U)):0;
 }
}
static void DELAY(unsigned us) { assert(us==1 || us==100);delays++; }
static phandle_t ofw_bus_get_node(device_t d) { assert(d==1);return 42; }
static bool OF_hasprop(phandle_t n,const char *name) {
 assert(n==42 && !strcmp(name,"assigned-clock-parents"));return assigned_parents;
}
static int OF_getproplen(phandle_t n,const char *name) {
 assert(n==42);
 return !strcmp(name,"assigned-clocks")?clock_len:!strcmp(name,"assigned-clock-rates")?rate_len:-1;
}
static int OF_getencprop(phandle_t n,const char *name,void *out,size_t len) {
 assert(n==42);
 int size=OF_getproplen(n,name);
 if(size<=0 || (unsigned)size>len)return -1;
 memcpy(out,!strcmp(name,"assigned-clocks")?property_clocks:property_rates,size);
 return size;
}
static phandle_t OF_node_from_xref(phandle_t xref) {return xref==2?42:99;}
'''

MAIN = r'''
/* A minimal dependency cache models the framework invalidation boundary. */
static int clknode_get_freq(struct clknode *c,uint64_t *frequency) {
 if(c==&xosc) {*frequency=50000000;return 0;}
 if(c->frequency) {*frequency=c->frequency;return 0;}
 const char *name=clknode_get_parent_names(c)[c->parent];
 int e=clknode_get_freq(clknode_find_by_name(name),frequency);
 if(e)return e;
 c->recalcs++;
 e=rp1_clknode_recalc(c,frequency);
 if(!e)c->frequency=*frequency;
 return e;
}
static struct clknode *id(int id) {return clknode_find_by_id(&domain,id);}
static void invalidate(void) {for(unsigned i=0;i<RP1_CLOCK_COUNT;i++)nodes[i].frequency=0;}
static void reset(void) {
 memset(&sc,0,sizeof(sc));memset(nodes,0,sizeof(nodes));memset(regs,0,sizeof(regs));
 sc.mem=&mem;sc.domain=&domain;sc.xosc_frequency=50000000;
 writes=reads=delays=reparents=pending_feedback=0;no_lock=no_select=reject_write=false;
 fail_reg=UINT32_MAX;
 for(unsigned i=0;i<RP1_CLOCK_COUNT;i++)snprintf(sc.names[i],48,"clock_%d",rp1_clocks[i].id);
 for(unsigned i=0;i<RP1_CLOCK_COUNT;i++) {
  nodes[i].sc.provider=&sc;nodes[i].sc.def=&rp1_clocks[i];sc.nodes[i]=&nodes[i];
  for(unsigned j=0;j<rp1_clocks[i].nparents;j++) {
   int p=rp1_clock_index(rp1_clocks[i].parents[j]);
   sc.parents[i][j]=p<0?"xosc":sc.names[p];
  }
  assert(rp1_clknode_init(&nodes[i],1)==0);
 }
 for(unsigned i=0;i<nitems(assigned_ids);i++) {
  property_clocks[2*i]=2;property_clocks[2*i+1]=assigned_ids[i];
  property_rates[i]=assigned_rates[i];
 }
 clock_len=sizeof(property_clocks);rate_len=sizeof(property_rates);assigned_parents=false;
}
static int set(int which,uint64_t requested,int flags,uint64_t *actual) {
 struct clknode *c=id(which);uint64_t fin;int done=0;
 int e=clknode_get_freq(clknode_find_by_name(clknode_get_parent_names(c)[c->parent]),&fin);
 if(e)return e;
 *actual=requested;e=rp1_clknode_set_freq(c,fin,actual,flags,&done);
 assert(done==1);
 if(!(flags&CLK_SET_DRYRUN))invalidate();
 return e;
}
static void configure(void) {
 uint64_t actual,readback;
 assert(rp1_clock_assignments(1)==0 && sc.nassigned==nitems(assigned_ids));
 for(unsigned i=0;i<sc.nassigned;i++) {
  assert(set(sc.assigned[i],sc.rates[i],CLK_SET_ROUND_ANY,&actual)==0);
  assert(rp1_clock_distance(actual,sc.rates[i])<=1);
  assert(clknode_get_freq(id(sc.assigned[i]),&readback)==0 && actual==readback);
  /* The real framework refreshes every descendant, including unassigned ones. */
  invalidate();
  for(unsigned j=0;j<RP1_CLOCK_COUNT;j++)assert(clknode_get_freq(&nodes[j],&readback)==0);
 }
 assert(rp1_clock_verify(&sc)==0);
 sc.ready=true;
}
int main(void) {
 struct rp1_clock_rate plan;
 uint64_t actual,before,freq;
 struct clknode *clk;
 phandle_t cells[2]={16,0};
 reset();regs[0x64/4]=0x10;configure();
 assert((regs[0x64/4]&0x10)!=0); /* Preserve bits outside SRC/AUXSRC. */
 assert(id(12)->parent==1 && reparents==1);
 assert((regs[0x64/4]&RP1_CLOCK_SRC)==1); /* AUX selected even when parent stays index0. */
 assert(rp1_clock_mapper(&domain,1,cells,&clk)==0 && clk==id(16));
 assert(rp1_clock_mapper(&domain,2,cells,&clk)==EINVAL);
 cells[0]=999;assert(rp1_clock_mapper(&domain,1,cells,&clk)==EINVAL);cells[0]=16;
 const unsigned speeds[]={125000000,25000000,2500000,125000000};
 for(unsigned i=0;i<nitems(speeds);i++) {
  assert(set(16,speeds[i],0,&actual)==0 && actual==speeds[i]);
  assert(clknode_get_freq(id(16),&freq)==0 && freq==speeds[i]);
  assert(clknode_get_freq(id(12),&freq)==0 && freq==200000000);
  assert(rp1_clknode_gate(id(16),true)==0);
  assert((regs[0x64/4]&RP1_CLOCK_ENABLE)!=0);
  assert(rp1_clknode_gate(id(16),false)==0);
 }
 before=writes;
 assert(set(0,1000000000,0,&actual)==0 && writes==before); /* No needless PLL restart. */
 assert(set(16,25000000,CLK_SET_DRYRUN,&actual)==0 && actual==25000000 && writes==before);
 assert(set(16,126000000,0,&actual)==ERANGE && writes==before);
 assert(set(16,0,0,&actual)==ERANGE && writes==before);
 assert(set(16,12345678,0,&actual)==ERANGE && writes==before);
 assert(set(0,1,CLK_SET_ROUND_ANY,&actual)==ERANGE && writes==before);
 assert(set(6,12345678,0,&actual)==ERANGE && writes==before);
 /* Impossible integer division with exact rounding must not be approximated. */
 assert(rp1_clock_rate_plan(RP1_LEAF,125000000,12345678,0,255,&plan)==ERANGE);
 assert(rp1_clock_rate_plan(RP1_LEAF,125000000,12345678,CLK_SET_ROUND_DOWN,255,&plan)==0 && plan.actual<=12345678);
 assert(rp1_clock_rate_plan(RP1_LEAF,125000000,12345678,CLK_SET_ROUND_UP,255,&plan)==0 && plan.actual>=12345678);
 assert(rp1_clock_rate_plan(RP1_LEAF,UINT64_MAX,1,3,255,&plan)==ERANGE);
 assert(rp1_clock_rate_plan(RP1_CORE,0,1000000000,3,0,&plan)==ERANGE);
 assert(rp1_clock_rate_plan(RP1_CORE,50000000,UINT64_MAX,3,0,&plan)==ERANGE);
 assert(rp1_clock_rate_plan(RP1_CORE,50000000,1000000000,4,0,&plan)==EOPNOTSUPP);
 assert(rp1_clock_rate_plan(RP1_CORE,50000000,1536000000,CLK_SET_ROUND_UP,0,&plan)==0 && plan.actual>=1536000000);
 assert(rp1_clock_rate_plan(RP1_CORE,50000000,1536000000,CLK_SET_ROUND_DOWN,0,&plan)==0 && plan.actual<=1536000000);
 assert(rp1_clock_rate_plan(RP1_PRIMARY,1000000000,200000000,0,0,&plan)==0 && plan.value*plan.extra==5);
 assert(rp1_clock_rate_plan(RP1_SECONDARY,1000000000,125000000,0,0,&plan)==0 && plan.value==8);
 assert(rp1_clock_rate_plan(RP1_SECONDARY,1000000000,200000000,0,0,&plan)==ERANGE);
 /* A disabled fractional modulator ignores a stale fractional register. */
 regs[0xc004/4]|=RP1_PLL_DSMPD;freq=50000000;
 assert(rp1_clknode_recalc(id(1),&freq)==0 && freq==1500000000);
 regs[0xc004/4]&=~RP1_PLL_DSMPD;
 /* Approximate or unavailable assigned rates prevent successful verification. */
 property_rates[0]+=1000;
 sc.rates[0]+=1000;assert(rp1_clock_verify(&sc)==ERANGE);sc.rates[0]-=1000;
 assert(rp1_clock_verify(&sc)==0);
 /* Invalid parent selection and zero dividers are not reported as valid rates. */
 regs[0x20/4]=3;assert(rp1_clock_parent(&sc,id(12)->sc.def)==-1);
 regs[0x20/4]=4;regs[0x68/4]=0;freq=125000000;
 assert(rp1_clknode_recalc(id(16),&freq)==0 && freq==0);
 /* Gate/select and PLL-lock timeouts poison new lookups and future writes. */
 reset();configure();no_select=true;regs[0x70/4]=0;
 assert(rp1_clknode_gate(id(16),true)==ETIMEDOUT && sc.poisoned && !sc.ready);
 assert(rp1_clock_mapper(&domain,1,cells,&clk)==ENXIO);
 before=writes;assert(set(16,25000000,0,&actual)==EIO && writes==before);
 reset();no_lock=true;
 assert(set(0,1000000000,CLK_SET_ROUND_ANY,&actual)==ETIMEDOUT && sc.poisoned);
 reset();configure();reject_write=true;fail_reg=0x68;
 assert(set(16,25000000,0,&actual)==EIO && sc.poisoned && !sc.ready);
 /* Assignments are validated before any MMIO writes. */
 reset();before=writes;clock_len=sizeof(property_clocks)-1;
 assert(rp1_clock_assignments(1)==EINVAL && writes==before);
 reset();assigned_parents=true;assert(rp1_clock_assignments(1)==EOPNOTSUPP && writes==0);
 reset();rate_len-=4;assert(rp1_clock_assignments(1)==EINVAL && writes==0);
 reset();property_clocks[0]=77;assert(rp1_clock_assignments(1)==EINVAL && writes==0);
 reset();property_clocks[1]=999;assert(rp1_clock_assignments(1)==EINVAL && writes==0);
 reset();property_clocks[3]=0;assert(rp1_clock_assignments(1)==EINVAL && writes==0);
 reset();property_rates[2]=0;assert(rp1_clock_assignments(1)==EINVAL && writes==0);
 puts("PASS: RP1 assigned clock graph/rates, Ethernet rates, dry runs, MMIO failures and timeout quarantine");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    source = SOURCE.read_text()
    declarations = source[source.index('#define RP1_CLOCK_XOSC'):source.index('static int\nrp1_clock_index')]
    callbacks = '\n'.join(function(source, name) for name in (
        'rp1_clock_index', 'rp1_clock_read', 'rp1_clock_write', 'rp1_clock_wait',
        'rp1_clock_parent', 'rp1_clknode_init', 'rp1_clknode_recalc',
        'rp1_clknode_mux', 'rp1_clknode_set_freq', 'rp1_clknode_gate',
        'rp1_clock_mapper', 'rp1_clock_assignments', 'rp1_clock_verify'))
    with tempfile.TemporaryDirectory(prefix='rp1-clock-controls-') as tmp:
        path = Path(tmp)
        (path / 'controls.c').write_text(PREFIX + '\n#include "rp1_clock_policy.h"\n' + declarations + MOCKS + callbacks + MAIN)
        subprocess.run([args.cc, '-std=gnu17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-sign-compare',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(SOURCE.parent), str(path / 'controls.c'),
                        '-o', str(path / 'controls')], check=True)
        subprocess.run([str(path / 'controls')], check=True)


if __name__ == '__main__':
    main()
