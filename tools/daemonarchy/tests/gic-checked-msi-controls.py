#!/usr/bin/env python3
"""Execute production GICv2 claim/map/setup/teardown and legacy MSI paths."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
SOURCE=ROOT/'sys/arm/arm/gic.c'

def block(text,pattern):
 m=re.search(pattern,text,re.M);assert m,pattern
 body=text.index('{',m.start());i=body+1;depth=1
 while depth:depth+=(text[i]=='{')-(text[i]=='}');i+=1
 if text[i:i+1]==';':i+=1
 return text[m.start():i]

PREFIX=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define FDT 1
#define DEV_ACPI 1
typedef unsigned u_int;
typedef uint32_t pcell_t;
typedef int device_t;
enum intr_polarity { INTR_POLARITY_CONFORM,INTR_POLARITY_HIGH,INTR_POLARITY_LOW };
enum intr_trigger { INTR_TRIGGER_CONFORM,INTR_TRIGGER_EDGE,INTR_TRIGGER_LEVEL };
struct intr_irqsrc { unsigned isrc_handlers,isrc_flags,isrc_cpu; };
struct mtx { bool held; };
struct resource { int unused; };
struct intr_map_data { unsigned type; };
struct intr_map_data_fdt { struct intr_map_data hdr;int iparent;unsigned ncells;pcell_t cells[3]; };
struct intr_map_data_acpi { struct intr_map_data hdr;unsigned irq;enum intr_polarity pol;enum intr_trigger trig; };
struct intr_map_data_msi { struct intr_map_data hdr;struct intr_irqsrc *isrc; };
#define INTR_MAP_DATA_FDT 1
#define INTR_MAP_DATA_ACPI 2
#define INTR_MAP_DATA_MSI 3
#define INTR_ISRCF_PPI 2
#define GIC_FIRST_SPI 32
#define GIC_FIRST_PPI 16
#define GIC_LAST_PPI 31
#define FDT_INTR_LOW_MASK 10
#define FDT_INTR_EDGE_MASK 3
#define KASSERT(x,message) assert(x)
#define powerof2(x) (((x)&((x)-1))==0)
#define CPU_SET(c,p) (*(p)|=1U<<(c))
#define PCPU_GET(x) 0
#define GIC_INTR_ISRC(sc,irq) (&(sc)->gic_irqs[irq].gi_isrc)
static void (*before_lock)(void);
static void mtx_lock_spin(struct mtx *m) { if(before_lock)before_lock();assert(!m->held);m->held=true; }
static void mtx_unlock_spin(struct mtx *m) { assert(m->held);m->held=false; }
static void device_printf(device_t d,const char *s,...) { (void)d;(void)s; }
'''
SERVICES=r'''
struct arm_gic_softc { struct mtx mutex;struct gic_irqsrc *gic_irqs;unsigned nirqs; };
static struct arm_gic_softc sc;
static struct gic_irqsrc irqs[256];
static unsigned configs;
static void *device_get_softc(device_t d) { assert(d==1);return &sc; }
static void gic_config(struct arm_gic_softc *s,unsigned irq,enum intr_trigger trig,enum intr_polarity pol) {
 assert(s==&sc && irq<256 && !sc.mutex.held && trig!=INTR_TRIGGER_CONFORM && pol!=INTR_POLARITY_CONFORM);configs++;
}
static int arm_gic_bind_intr(device_t d,struct intr_irqsrc *src) { assert(d==1 && src && !sc.mutex.held);return 0; }
'''
TEST=r'''
static void reset(void) {
 memset(irqs,0,sizeof(irqs));sc=(struct arm_gic_softc){.gic_irqs=irqs,.nirqs=256};
 for(unsigned i=0;i<256;i++)irqs[i].gi_irq=i;
 configs=0;before_lock=NULL;
}
static unsigned hook_count;
static void claim_between_map_and_commit(void) {
 if(++hook_count==2) { before_lock=NULL;assert(arm_gic_reserve_msi_range_checked(1,160,64)==0); }
}
int main(void) {
 struct intr_map_data_fdt fdt={.hdr={INTR_MAP_DATA_FDT},.ncells=3,.cells={0,128,1}};
 struct intr_map_data_acpi acpi={.hdr={INTR_MAP_DATA_ACPI},.irq=160,.pol=INTR_POLARITY_HIGH,.trig=INTR_TRIGGER_EDGE};
 struct intr_map_data_msi msi={.hdr={INTR_MAP_DATA_MSI}};
 struct intr_irqsrc *src,*all[64];unsigned irq;
 reset();
 assert(gic_reserve_msi_range_unsupported(1,160,64)==EOPNOTSUPP);
 assert(gic_map_intr(1,(void *)&fdt,&irq,NULL,NULL)==0 && irq==160);
 assert(arm_gic_map_intr(1,(void *)&fdt,&src)==0 && src==&irqs[160].gi_isrc);
 assert(arm_gic_reserve_msi_range_checked(1,160,64)==0);
 for(unsigned i=0;i<64;i++)assert((irqs[160+i].gi_flags&(GI_FLAG_MSI|GI_FLAG_MSI_CHECKED))==(GI_FLAG_MSI|GI_FLAG_MSI_CHECKED));
 assert(gic_map_intr(1,(void *)&fdt,&irq,NULL,NULL)==EBUSY);
 assert(gic_map_intr(1,(void *)&acpi,&irq,NULL,NULL)==EBUSY);
 assert(arm_gic_setup_intr(1,src,NULL,(void *)&fdt)!=0 && !configs);
 assert(arm_gic_setup_intr(1,src,NULL,NULL)==EBUSY);
 assert(arm_gic_reserve_msi_range_checked(1,160,64)==EBUSY);
 for(unsigned i=0;i<64;i++) {
  assert(arm_gic_alloc_msix(1,160,64,&all[i])==0 && all[i]==&irqs[160+i].gi_isrc);
  msi.isrc=all[i];assert(arm_gic_setup_intr(1,all[i],NULL,(void *)&msi)==0);
 }
 assert(arm_gic_alloc_msix(1,160,64,&src)==ENXIO);
 for(unsigned i=0;i<64;i++)assert(arm_gic_release_msix(1,all[i])==0);
 assert(configs==64 && !sc.mutex.held);
 /* Same resource can map before claim and then lose the setup race. */
 reset();hook_count=0;before_lock=claim_between_map_and_commit;
 assert(arm_gic_setup_intr(1,&irqs[160].gi_isrc,NULL,(void *)&fdt)==EBUSY && !configs && !sc.mutex.held);
 /* Complete validation prevents partial claims on a late busy source. */
 for(unsigned kind=0;kind<4;kind++) {
  reset();if(kind==0)irqs[223].gi_pol=INTR_POLARITY_HIGH;
  if(kind==1)irqs[223].gi_trig=INTR_TRIGGER_EDGE;
  if(kind==2)irqs[223].gi_flags=GI_FLAG_MSI;
  if(kind==3)irqs[223].gi_isrc.isrc_handlers=1;
  assert(arm_gic_reserve_msi_range_checked(1,160,64)==EBUSY);
  for(unsigned i=160;i<223;i++)assert(irqs[i].gi_flags==0 && irqs[i].gi_pol==INTR_POLARITY_CONFORM);
 }
 reset();assert(arm_gic_reserve_msi_range_checked(1,0,64)==EINVAL);
 assert(arm_gic_reserve_msi_range_checked(1,16,1)==EINVAL);
 assert(arm_gic_reserve_msi_range_checked(1,160,0)==EINVAL);
 assert(arm_gic_reserve_msi_range_checked(1,160,UINT_MAX)==EINVAL);
 assert(arm_gic_reserve_msi_range_checked(1,UINT_MAX,1)==EINVAL);
 /* Actual ordinary setup commits configuration before framework handler++;
  * teardown retains it until after framework handler--. */
 assert(arm_gic_setup_intr(1,&irqs[160].gi_isrc,NULL,(void *)&fdt)==0);
 assert(arm_gic_reserve_msi_range_checked(1,160,64)==EBUSY);
 irqs[160].gi_isrc.isrc_handlers=1;
 assert(arm_gic_teardown_intr(1,&irqs[160].gi_isrc,NULL,(void *)&fdt)==0);
 assert(arm_gic_reserve_msi_range_checked(1,160,64)==EBUSY);
 irqs[160].gi_isrc.isrc_handlers=0;
 assert(arm_gic_teardown_intr(1,&irqs[160].gi_isrc,NULL,(void *)&fdt)==0);
 assert(arm_gic_reserve_msi_range_checked(1,160,64)==0);
 /* Legacy v2m reserve, NULL setup and typed MSI setup remain valid. */
 reset();arm_gic_reserve_msi_range(1,160,64);
 assert(arm_gic_setup_intr(1,&irqs[160].gi_isrc,NULL,NULL)==0 && configs==1);
 msi.isrc=&irqs[161].gi_isrc;
 assert(arm_gic_setup_intr(1,msi.isrc,NULL,(void *)&msi)==0 && configs==2);
 assert(arm_gic_alloc_msi(1,160,64,8,8,all)==0);
 assert(arm_gic_release_msi(1,8,all)==0);
 assert(arm_gic_teardown_intr(1,msi.isrc,NULL,(void *)&msi)==0 && irqs[161].gi_pol==INTR_POLARITY_HIGH);
 puts("PASS: production GIC checked/legacy reservations, FDT+ACPI fail-closed mapping, setup race, all64 parent allocation/release, atomic busy validation, overflow/SGI/PPI refusal, ordinary setup/teardown and legacy NULL+MSI setup without nested locks");
}
'''
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='cc');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
 text=SOURCE.read_text();types=block(text,r'^struct gic_irqsrc\s*\{')
 names=['arm_gic_reserve_msi_range_checked','arm_gic_reserve_msi_range','gic_map_fdt','gic_map_msi','gic_map_intr','arm_gic_map_intr','arm_gic_setup_intr','arm_gic_teardown_intr','arm_gic_alloc_msi','arm_gic_release_msi','arm_gic_alloc_msix','arm_gic_release_msix']
 unsupported=block((ROOT/'sys/arm/arm/gic_if.m').read_text(),r'^\tstatic int\s+gic_reserve_msi_range_unsupported\(')
 funcs=unsupported+'\n'+'\n'.join(block(text,r'^static (?:int|void)\s+'+n+r'\([^;]*?\n\{') for n in names)
 with tempfile.TemporaryDirectory(prefix='gic-msi-') as tmp:
  path=Path(tmp)/'test';path.with_suffix('.c').write_text(PREFIX+types+SERVICES+funcs+TEST)
  cmd=[a.cc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare',str(path.with_suffix('.c')),'-o',str(path)]
  if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd,check=True);subprocess.run([str(path)],check=True)
if __name__=='__main__':main()
