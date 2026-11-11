/* SPDX-License-Identifier: BSD-2-Clause */
/* Production C policy controls; the fixture contains only enrolled DTB cells. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bcm2712_dma_policy.h"
#include "selected-dma-fixture.h"

static struct bcm2712_dma_constraints
constraints(uint64_t mask)
{
	return ((struct bcm2712_dma_constraints) { .device_mask=mask,
	    .alignment=1, .boundary=0, .max_segment_length=UINT64_MAX,
	    .max_segments=BCM2712_DMA_MAX_SEGMENTS, .role=BCM2712_DMA_RAM });
}

static void
map_failure(const struct bcm2712_dma_policy *p,
    const struct bcm2712_dma_constraints *c,
    const struct bcm2712_dma_cpu_span *spans, unsigned count,
    unsigned capacity, int error)
{
	struct bcm2712_dma_device_segment out[BCM2712_DMA_MAX_SEGMENTS], before[BCM2712_DMA_MAX_SEGMENTS];
	unsigned n=0xaaaa;
	memset(out,0xa5,sizeof(out));memcpy(before,out,sizeof(out));
	assert(bcm2712_dma_policy_map(p,c,spans,count,out,capacity,&n)==error);
	assert(n==0xaaaa && !memcmp(out,before,sizeof(out)));
}

static struct bcm2712_dma_policy
root(const struct bcm2712_dma_root_window *windows,unsigned count)
{
	struct bcm2712_dma_policy p;
	assert(bcm2712_dma_policy_root(&p,windows,count)==0);
	return (p);
}

static unsigned
map_ok(const struct bcm2712_dma_policy *p,
    const struct bcm2712_dma_constraints *c,
    const struct bcm2712_dma_cpu_span *spans,unsigned count,
    struct bcm2712_dma_device_segment *out)
{
	unsigned n=0xaaaa;
	struct bcm2712_dma_policy before=*p;
	assert(bcm2712_dma_policy_map(p,c,spans,count,out,BCM2712_DMA_MAX_SEGMENTS,&n)==0);
	assert(!memcmp(&before,p,sizeof(before)));
	return (n);
}

static void
test_enrolled(void)
{
	struct bcm2712_dma_policy host=root(selected_root,2),rp1;
	struct bcm2712_dma_constraints c=constraints(UINT64_MAX);
	struct bcm2712_dma_cpu_span span={0x1234,0x2000};
	struct bcm2712_dma_device_segment out[BCM2712_DMA_MAX_SEGMENTS];
	struct bcm2712_dma_eligible_window eligible[BCM2712_DMA_MAX_RANGES];
	unsigned n;
	assert(bcm2712_dma_policy_compose(&rp1,&host,selected_child,3)==0 && rp1.count==3);
	assert(rp1.ranges[0].cpu_base==0 && rp1.ranges[0].device_base==UINT64_C(0x1000000000));
	assert(rp1.ranges[1].cpu_base==UINT64_C(0x1f00000000) && rp1.ranges[1].length==0x400000);
	assert(rp1.ranges[2].cpu_base==0 && rp1.ranges[2].device_base==0 && rp1.ranges[2].length==UINT64_C(0x1000000000));
	assert(rp1.ranges[2].child_space_flags==0x02000000 && rp1.ranges[2].root_space_flags==0x43000000);
	assert(map_ok(&host,&c,&span,1,out)==1 && out[0].device_base==UINT64_C(0x1000001234));
	assert(map_ok(&rp1,&c,&span,1,out)==1 && out[0].device_base==UINT64_C(0x1000001234) && out[0].range_index==0);
	c.device_mask=UINT32_MAX;
	assert(map_ok(&rp1,&c,&span,1,out)==1 && out[0].device_base==span.cpu_base && out[0].range_index==2);
	map_failure(&host,&c,&span,1,64,EFBIG);
	assert(bcm2712_dma_policy_eligible(&host,&c,NULL,0,&n)==0 && n==0);
	assert(bcm2712_dma_policy_eligible(&rp1,&c,eligible,32,&n)==0 && n==1);
	assert(eligible[0].cpu_base==0 && eligible[0].device_base==0 && eligible[0].length==UINT64_C(0x100000000));
	span.cpu_base=UINT64_C(0x100000000);span.length=1;
	map_failure(&rp1,&c,&span,1,64,EFBIG); /* Low physical bounce preimage, not a RAM cap. */
	span.cpu_base=selected_msi.cpu_doorbell_addr;
	map_failure(&host,&c,&span,1,64,ENXIO);
	assert(selected_msi.pci_message_addr==UINT64_C(0xfffffff000) && selected_msi.cpu_doorbell_addr==UINT64_C(0x1000130000));
	c=constraints(UINT64_MAX);c.role=BCM2712_DMA_PEER;
	span.cpu_base=UINT64_C(0x1f003fffff);span.length=1;
	assert(map_ok(&rp1,&c,&span,1,out)==1 && out[0].device_base==UINT64_C(0xc0403fffff));
	span.cpu_base++;map_failure(&rp1,&c,&span,1,64,ENXIO);
	c.role=BCM2712_DMA_RAM;span.cpu_base=UINT64_C(0x1f00000000);
	map_failure(&rp1,&c,&span,1,64,ENXIO);
	puts("PASS enrolled DMA geometry: host64GiB offset, both RP1 aliases,32bit preimage, clipped peer tail, no MSI/RAM conflation");
}

static void
test_alignment_and_canonical_admission(void)
{
	const struct bcm2712_dma_root_window windows[]={
	 {0,0,3,BCM2712_DMA_RAM,0x02000000},{3,4,4,BCM2712_DMA_RAM,0x02000000}};
	struct bcm2712_dma_policy p=root(windows,2);
	struct bcm2712_dma_constraints c=constraints(UINT64_MAX);
	struct bcm2712_dma_cpu_span span={0,7};
	struct bcm2712_dma_device_segment out[64];
	c.alignment=4;
	assert(map_ok(&p,&c,&span,1,out)==2 && out[0].length==3 && out[1].device_base==4 && out[1].length==4);
	const struct bcm2712_dma_root_window dead_end[]={
	 {0,0,6,BCM2712_DMA_RAM,0x02000000},{4,8,4,BCM2712_DMA_RAM,0x02000000},
	 {6,16,1,BCM2712_DMA_RAM,0x02000000}};
	p=root(dead_end,3);span.length=8;
	map_failure(&p,&c,&span,1,64,EFBIG);
	/* A4+B4 is a legal alternative, but greedy A6 enters a dead end.
	 * This named refusal is an explicit bounded admission, not impossibility.
	 */
	struct bcm2712_dma_cpu_span witness[]={{0,4},{4,4}};
	assert(map_ok(&p,&c,witness,2,out)==2 && out[0].length==4 && out[1].device_base==8);
	const struct bcm2712_dma_root_window congruent[]={
	 {3,4,8,BCM2712_DMA_RAM,0x02000000}};
	p=root(congruent,1);
	struct bcm2712_dma_eligible_window eligible[32];unsigned n;
	assert(bcm2712_dma_policy_eligible(&p,&c,eligible,32,&n)==0 && n==1);
	assert(eligible[0].start_modulus==4 && eligible[0].start_remainder==3 && eligible[0].length==8);
	span=(struct bcm2712_dma_cpu_span){3,8};assert(map_ok(&p,&c,&span,1,out)==1);
	span=(struct bcm2712_dma_cpu_span){4,1};map_failure(&p,&c,&span,1,64,EFBIG);
	puts("PASS alias alignment transitions and explicit canonical dead-end admission with independently legal alternative");
}

static void
test_segments(void)
{
	const struct bcm2712_dma_root_window window={0,0,1024,BCM2712_DMA_RAM,0x02000000};
	struct bcm2712_dma_policy p=root(&window,1);
	struct bcm2712_dma_constraints c=constraints(UINT64_MAX);
	struct bcm2712_dma_cpu_span span={0,10},parts[]={{0,2},{2,2}};
	struct bcm2712_dma_device_segment out[64];
	c.max_segment_length=3;c.boundary=4;
	assert(map_ok(&p,&c,&span,1,out)==5);
	const uint64_t sizes[]={3,1,3,1,2};
	for(unsigned i=0;i<5;i++)assert(out[i].length==sizes[i]);
	map_failure(&p,&c,&span,1,4,EFBIG);
	c.max_segments=4;map_failure(&p,&c,&span,1,64,EFBIG);
	c=constraints(UINT64_MAX);
	assert(map_ok(&p,&c,parts,2,out)==1 && out[0].length==4);
	parts[1].cpu_base=3;assert(map_ok(&p,&c,parts,2,out)==2);
	c.max_segment_length=1;span.length=65;map_failure(&p,&c,&span,1,64,EFBIG);
	const struct bcm2712_dma_root_window holes[]={
	 {0,0,2,BCM2712_DMA_RAM,0x02000000},{4,4,2,BCM2712_DMA_RAM,0x02000000}};
	p=root(holes,2);span=(struct bcm2712_dma_cpu_span){0,6};
	map_failure(&p,&c,&span,1,64,ENXIO);
	puts("PASS segmentation, boundary/max-segment limits, safe coalescing, output budgets and real holes");
}

static void
test_validation_and_composition(void)
{
	struct bcm2712_dma_root_window w={UINT64_MAX,UINT64_MAX,1,BCM2712_DMA_RAM,0x03000000};
	struct bcm2712_dma_policy p=root(&w,1),out,before;
	struct bcm2712_dma_constraints c=constraints(UINT64_MAX);
	struct bcm2712_dma_cpu_span span={UINT64_MAX,1};
	struct bcm2712_dma_device_segment segs[64];
	assert(map_ok(&p,&c,&span,1,segs)==1 && segs[0].device_base==UINT64_MAX);
	memset(&out,0xa5,sizeof(out));before=out;
	w.length=2;assert(bcm2712_dma_policy_root(&out,&w,1)==EINVAL && !memcmp(&out,&before,sizeof(out)));
	w.length=0;assert(bcm2712_dma_policy_root(&out,&w,1)==EINVAL && !memcmp(&out,&before,sizeof(out)));
	struct bcm2712_dma_child_window child={7,UINT64_MAX,1,0x02000000};
	assert(bcm2712_dma_policy_compose(&out,&p,&child,1)==0 && out.ranges[0].cpu_base==UINT64_MAX && out.ranges[0].device_base==7);
	before=out;child.length=2;
	assert(bcm2712_dma_policy_compose(&out,&p,&child,1)==EINVAL && !memcmp(&out,&before,sizeof(out)));
	w=(struct bcm2712_dma_root_window){0,10,10,BCM2712_DMA_RAM,0x43000000};p=root(&w,1);
	child=(struct bcm2712_dma_child_window){100,15,10,0x02000000};
	assert(bcm2712_dma_policy_compose(&out,&p,&child,1)==0 && out.ranges[0].cpu_base==5 && out.ranges[0].device_base==100 && out.ranges[0].length==5);
	before=out;child.parent_bus_base=30;
	assert(bcm2712_dma_policy_compose(&out,&p,&child,1)==ENXIO && !memcmp(&out,&before,sizeof(out)));
	struct bcm2712_dma_root_window conflict[]={
	 {0,0,4,BCM2712_DMA_RAM,0x02000000},{0,8,4,BCM2712_DMA_PEER,0x02000000}};
	assert(bcm2712_dma_policy_root(&out,conflict,2)==EINVAL && !memcmp(&out,&before,sizeof(out)));
	conflict[1]=(struct bcm2712_dma_root_window){4,0,4,BCM2712_DMA_RAM,0x02000000};
	assert(bcm2712_dma_policy_root(&out,conflict,2)==EINVAL);
	w=(struct bcm2712_dma_root_window){0,0,16,BCM2712_DMA_RAM,0x02000000};p=root(&w,1);span=(struct bcm2712_dma_cpu_span){0,1};
	for(unsigned bad=0;bad<6;bad++) {
	 c=constraints(UINT64_MAX);
	 if(bad==0)c.alignment=0;if(bad==1)c.boundary=3;if(bad==2)c.device_mask=5;
	 if(bad==3)c.max_segment_length=0;if(bad==4)c.max_segments=65;if(bad==5)c.role=0;
	 map_failure(&p,&c,&span,1,64,EINVAL);
	}
	c=constraints(0);assert(map_ok(&p,&c,&span,1,segs)==1);span.length=2;map_failure(&p,&c,&span,1,64,EFBIG);
	span=(struct bcm2712_dma_cpu_span){UINT64_MAX,2};map_failure(&p,&c,&span,1,64,EINVAL);
	span.length=0;map_failure(&p,&c,&span,1,64,EINVAL);
	assert(map_ok(&p,&c,NULL,0,segs)==0);
	puts("PASS inclusive MAX endpoint, mask0, zero/wrap rejection, malformed constraints, disjoint device space and clipped composition");
}

static void
test_capacity(void)
{
	struct bcm2712_dma_root_window windows[32];
	struct bcm2712_dma_policy p,out,before;
	for(unsigned i=0;i<32;i++)windows[i]=(struct bcm2712_dma_root_window){i*2,i*2,1,BCM2712_DMA_RAM,0x02000000};
	p=root(windows,32);
	struct bcm2712_dma_child_window child[]={{0,0,64,0x02000000},{128,0,64,0x02000000}};
	memset(&out,0xa5,sizeof(out));before=out;
	assert(bcm2712_dma_policy_compose(&out,&p,child,2)==EFBIG && !memcmp(&out,&before,sizeof(out)));
	struct bcm2712_dma_constraints c=constraints(UINT64_MAX);
	struct bcm2712_dma_eligible_window e[32],saved[32];unsigned n=0xaaaa;
	memset(e,0xa5,sizeof(e));memcpy(saved,e,sizeof(e));
	assert(bcm2712_dma_policy_eligible(&p,&c,e,31,&n)==EFBIG && n==0xaaaa && !memcmp(e,saved,sizeof(e)));
	puts("PASS bounded composition/query capacity faults retain all output bytes/counts");
}
int main(void)
{
	test_enrolled();test_alignment_and_canonical_admission();test_segments();
	test_validation_and_composition();test_capacity();return (0);
}
