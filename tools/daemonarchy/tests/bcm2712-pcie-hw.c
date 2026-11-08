/* SPDX-License-Identifier: BSD-2-Clause */
/* Host injected protocol primitives. This is not a BCM2712 hardware model. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <arm64/broadcom/bcm2712_pcie_hw.h>
#include "selected-pcie2.h"
struct unit {
 uint32_t regs[0x9310/4];
 unsigned calls, fail_at, delay_us, dependencies, publications, mdio_commands;
 unsigned mdio_reg, mdio_pos, bridge_calls, pulses;
 bool mdio_timeout, link_timeout, qos_reserved, drop, partial_child;
 unsigned drop_at;
};
static int fault(struct unit *u) { return ++u->calls==u->fail_at?EIO:0; }
static int read_reg(void *arg,uint32_t reg,unsigned width,uint32_t *value) {
 struct unit *u=arg;int error=fault(u);if(error)return error;
 assert((width==2||width==4) && reg+width<=0x9310 && reg%width==0);
 if(reg==0x1104 && !u->mdio_timeout)u->regs[reg/4]&=0x7fffffff;
 if(reg==0x4068 && !u->link_timeout && u->delay_us>=100500 && (u->regs[0x4064/4]&4))u->regs[reg/4]|=0x30;
 *value=u->regs[reg/4]>>((reg%4)*8);if(width==2)*value&=0xffff;return 0;
}
static int write_reg(void *arg,uint32_t reg,unsigned width,uint32_t value) {
 static const uint16_t regs[]={0x1f,0x16,0x17,0x18,0x19,0x1b,0x1c,0x1e};
 static const uint16_t values[]={0x1600,0x50b9,0xbda1,0x0094,0x97b4,0x5030,0x5030,0x0007};
 struct unit *u=arg;int error=fault(u);if(error)return error;
 assert((width==2||width==4)&&reg+width<=0x9310&&reg%width==0);
 if(u->drop && u->calls==u->drop_at && reg!=0x1100 && reg!=0x1104)return 0;
 if(reg==0x1100){assert(u->mdio_pos<8 && value==regs[u->mdio_pos]);u->mdio_reg=value;u->mdio_commands++;}
 if(reg==0x1104){assert(u->mdio_pos<8 && u->mdio_reg==regs[u->mdio_pos] && value==(0x80000000U|values[u->mdio_pos]));u->mdio_pos++;}
 if(reg==0x416c && u->qos_reserved)value&=~(1U<<12);
 if(width==4)u->regs[reg/4]=value;
 else {unsigned shift=(reg%4)*8;u->regs[reg/4]=(u->regs[reg/4]&~(0xffffU<<shift))|((value&0xffff)<<shift);}
 return 0;
}
static int reset(void *arg,enum bcm2712_pcie_reset which,int asserted) {
 struct unit *u=arg;int error=fault(u);if(error)return error;
 if(which==BCM2712_PCIE_RESCAL){if(!asserted){assert(u->pulses==0);u->pulses++;}}
 else {static const int states[]={0,1,0};assert(u->bridge_calls<3 && asserted==states[u->bridge_calls++]);}
 return 0;
}
static void delay(void *arg,unsigned usec){struct unit *u=arg;assert(usec<=100000);u->delay_us+=usec;}
static int dependencies(void *arg){struct unit *u=arg;int error=fault(u);if(error)return error;u->dependencies++;return 0;}
static int publish(void *arg){struct unit *u=arg;int error=fault(u);if(error)return error;assert(u->dependencies==2 && u->pulses==1 && u->bridge_calls==3 && u->mdio_pos==8 && u->delay_us>=100500);u->publications++;return u->partial_child?EIO:0;}
static const struct bcm2712_pcie_ops ops={read_reg,write_reg,reset,delay,dependencies,publish};
static void initialize(struct unit *u){memset(u,0,sizeof(*u));u->regs[0x4068/4]=0x80;u->regs[0x4008/4]=0x78000000;u->regs[0xbc/4]=0x42U<<16;}
int main(void){
 struct bcm2712_pcie_dt d=selected_dt();struct bcm2712_pcie_plan plan,poison;
 memset(&plan,0xa5,sizeof(plan));assert(bcm2712_pcie_plan_make(&d,&plan)==0);
 assert(plan.outbound[0].length==0xfffffffc && plan.out[0].decode_last==UINT64_C(0x1fffffffff));
 assert(plan.out[0].base_limit==0xfff00000 && plan.out[0].base_hi==0x1f && plan.out[0].limit_hi==0x1f);
 assert(plan.out[1].pci_hi==4 && plan.out[1].base_hi==0x1c && plan.out[1].limit_hi==0x1e);
 assert(plan.in[0].pci_lo==7 && plan.in[1].pci_lo==0x15 && plan.in[2].pci_lo==0xfffff01c);
 assert(plan.in[2].pci_hi==0xff && plan.in[2].cpu_lo==0x130001 && plan.in[2].cpu_hi==0x10 && plan.qos==0xbbaa9888);
 assert(plan.raw_msi.pci_message_addr==UINT64_C(0xfffffff000) && plan.ram_policy.count==2);
 for(unsigned bad=0;bad<17;bad++){
  struct bcm2712_pcie_dt x=d;memset(&poison,0xa5,sizeof(poison));struct bcm2712_pcie_plan saved=poison;
  switch(bad){case 0:x.nreg--;break;case 1:x.nranges++;break;case 2:x.ndma--;break;case 3:x.nresets--;break;case 4:x.ninterrupts++;break;case 5:x.ninterrupt_map--;break;case 6:x.domain=0;break;case 7:x.lanes=1;break;case 8:x.generation=3;break;case 9:x.msi_parent=1;break;case 10:x.parent_acells=1;break;case 11:x.parent_scells=1;break;case 12:x.icells=3;break;case 13:x.interrupt_mask[3]=3;break;case 14:x.qos[6]=10;break;case 15:x.unsupported=1;break;case 16:x.ranges=NULL;break;}
  assert(bcm2712_pcie_plan_make(&x,&poison)!=0 && memcmp(&poison,&saved,sizeof(poison))==0);
 }

 struct bcm2712_pcie_window extreme={UINT64_MAX-4095,0,4096};
 struct bcm2712_pcie_inbound ib,ibs;memset(&ib,0xa5,sizeof(ib));ibs=ib;
 assert(bcm2712_pcie_encode_inbound(&extreme,&ib)==0 && ib.pci_hi==UINT32_MAX && ib.pci_lo==0xfffff01c);
 extreme=(struct bcm2712_pcie_window){0,UINT64_MAX-4095,4096};ib=ibs;
 assert(bcm2712_pcie_encode_inbound(&extreme,&ib)==EOPNOTSUPP && memcmp(&ib,&ibs,sizeof(ib))==0);
 struct bcm2712_pcie_outbound ob,obs;memset(&ob,0xa5,sizeof(ob));obs=ob;
 extreme=(struct bcm2712_pcie_window){UINT64_MAX-0xfffff,0,0x100000};
 assert(bcm2712_pcie_encode_outbound(&extreme,&ob)==0 && ob.pci_hi==UINT32_MAX && ob.pci_lo==0xfff00000);
 extreme=(struct bcm2712_pcie_window){0,UINT64_MAX-0xfffff,0x100000};ob=obs;
 assert(bcm2712_pcie_encode_outbound(&extreme,&ob)==EOVERFLOW && memcmp(&ob,&obs,sizeof(ob))==0);
 extreme=(struct bcm2712_pcie_window){UINT64_MAX-4095,0,8192};ib=ibs;
 assert(bcm2712_pcie_encode_inbound(&extreme,&ib)==EINVAL && memcmp(&ib,&ibs,sizeof(ib))==0);
 puts("PASS selected raw-cell plan, exact64-bit windows, envelope tail, QoS, malformed shape/output rollback");
 struct unit u;struct bcm2712_pcie_state state={0};initialize(&u);assert(bcm2712_pcie_start(&plan,&ops,&u,&state)==0 && state.published && state.link && !state.retained && state.generation==2 && state.lanes==4);assert((u.regs[0x4008/4]&0xf8000000)==0x78000000);unsigned total=u.calls;assert(bcm2712_pcie_start(&plan,&ops,&u,&state)==EBUSY && u.calls==total);
 for(unsigned fail=1;fail<=total;fail++){
  initialize(&u);u.fail_at=fail;state=(struct bcm2712_pcie_state){0};assert(bcm2712_pcie_start(&plan,&ops,&u,&state)==EIO && !state.published && !u.publications && state.retained==(fail!=1));
  if(state.touched){unsigned calls=u.calls;assert(bcm2712_pcie_start(&plan,&ops,&u,&state)==EBUSY && u.calls==calls);}
 }
 printf("PASS every %u register/provider/reset/publication fault suppresses children, retained touched ownership, no pulse retry\n",total);
 initialize(&u);u.mdio_timeout=true;state=(struct bcm2712_pcie_state){0};assert(bcm2712_pcie_start(&plan,&ops,&u,&state)==ETIMEDOUT && state.retained && !u.publications);
 initialize(&u);u.link_timeout=true;state=(struct bcm2712_pcie_state){0};assert(bcm2712_pcie_start(&plan,&ops,&u,&state)==ETIMEDOUT && state.retained && !u.publications);
 initialize(&u);u.qos_reserved=true;state=(struct bcm2712_pcie_state){0};assert(bcm2712_pcie_start(&plan,&ops,&u,&state)==0 && state.qos_fallback && (u.regs[0x416c/4]&0x3f)==15);
 initialize(&u);u.partial_child=true;state=(struct bcm2712_pcie_state){0};assert(bcm2712_pcie_start(&plan,&ops,&u,&state)==EIO && state.retained && state.publication_attempted && !state.published && u.publications==1);
 puts("PASS bounded MDIO/link timeouts, revision-dependent QoS fallback and partial-child retention");
 /* Drop every ordinary RW register write. Idempotent zero/default writes may
  * read back equal: acceptance there is correct. No mismatched readback may
  * publish; strict equality is conservative, not an invented HW mask. */
 unsigned detected=0;
 for(unsigned at=1;at<=total;at++){
  initialize(&u);u.drop=true;u.drop_at=at;state=(struct bcm2712_pcie_state){0};int e=bcm2712_pcie_start(&plan,&ops,&u,&state);if(e){assert(!state.published && state.retained);detected++;}
 }
 assert(detected>30);printf("PASS %u non-idempotent dropped writes/readback faults suppressed publication\n",detected);
 puts("PASS production PCIe2 encoder/state engine; no native BCM2712/RP1 model or hardware execution");return 0;
}
