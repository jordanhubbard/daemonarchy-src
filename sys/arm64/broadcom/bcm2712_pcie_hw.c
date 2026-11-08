/*
 * Copyright (c) 2020, 2025 Mark Kettenis <kettenis@openbsd.org>
 * Copyright (c) 2026 Jordan Hubbard
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#else
#include <stddef.h>
#include <string.h>
#endif
#include <sys/errno.h>
#include <arm64/broadcom/bcm2712_pcie_hw.h>

#define PLAN_VALID UINT32_C(0x27120002)
#define MIB (UINT64_C(1) << 20)
#define CPU_LIMIT ((UINT64_C(1) << 40) - 1)

static int
span_valid(uint64_t base, uint64_t length)
{
	return (length != 0 && length - 1 <= UINT64_MAX - base);
}

int
bcm2712_pcie_encode_outbound(const struct bcm2712_pcie_window *w,
    struct bcm2712_pcie_outbound *out)
{
	struct bcm2712_pcie_outbound stage = {0};
	uint64_t last;

	if (w == NULL || out == NULL || !span_valid(w->pci, w->length) ||
	    !span_valid(w->cpu, w->length) ||
	    ((w->cpu | w->pci) & (MIB - 1)) != 0)
		return (EINVAL);
	last = w->cpu + (w->length - 1);
	if (last > CPU_LIMIT)
		return (EOVERFLOW);
	stage.pci_lo = (uint32_t)w->pci;
	stage.pci_hi = (uint32_t)(w->pci >> 32);
	stage.base_limit = (uint32_t)((w->cpu >> 16) & 0xfff0) |
	    (uint32_t)(last & 0xfff00000);
	stage.base_hi = (uint32_t)(w->cpu >> 32);
	stage.limit_hi = (uint32_t)(last >> 32);
	stage.decode_first = w->cpu;
	stage.decode_last = last | (MIB - 1);
	*out = stage;
	return (0);
}

int
bcm2712_pcie_encode_inbound(const struct bcm2712_pcie_window *w,
    struct bcm2712_pcie_inbound *out)
{
	struct bcm2712_pcie_inbound stage = {0};
	unsigned shift;
	uint32_t code;

	if (w == NULL || out == NULL || !span_valid(w->pci, w->length) ||
	    !span_valid(w->cpu, w->length) ||
	    (w->length & (w->length - 1)) != 0 ||
	    ((w->pci | w->cpu) & (w->length - 1)) != 0)
		return (EINVAL);
	for (shift = 0; shift < 64 && (UINT64_C(1) << shift) != w->length; shift++)
		;
	if (shift < 12 || shift > 36 || w->cpu + (w->length - 1) > CPU_LIMIT)
		return (EOPNOTSUPP);
	code = shift < 16 ? 0x1c + shift - 12 : shift - 15;
	stage.pci_lo = (uint32_t)w->pci | code;
	stage.pci_hi = (uint32_t)(w->pci >> 32);
	stage.cpu_lo = (uint32_t)w->cpu | 1;
	stage.cpu_hi = (uint32_t)(w->cpu >> 32);
	*out = stage;
	return (0);
}

static uint64_t
pair(const uint32_t *v)
{
	return ((uint64_t)v[0] << 32 | v[1]);
}

int
bcm2712_pcie_plan_make(const struct bcm2712_pcie_dt *d,
    struct bcm2712_pcie_plan *out)
{
	static const uint32_t reg[] = {0x10, 0x120000, 0, 0x9310};
	static const uint32_t ranges[] = {0x02000000,0,0,0x1f,0,0,0xfffffffc,
	    0x43000000,4,0,0x1c,0,3,0};
	static const uint32_t dma[] = {0x02000000,0,0,0x1f,0,0,0x400000,
	    0x43000000,0x10,0,0,0,0x10,0,
	    0x03000000,0xff,0xfffff000,0x10,0x130000,0,0x1000};
	static const uint32_t resets[] = {0x23,0x24,44};
	static const uint32_t interrupts[] = {0,233,4,0,234,4};
	static const uint32_t imap[] = {0,0,0,1,1,0,229,4,0,0,0,2,1,0,230,4,
	    0,0,0,3,1,0,231,4,0,0,0,4,1,0,232,4};
	static const uint8_t qos[] = {8,8,8,9,10,10,11,11};
	struct bcm2712_pcie_plan stage = {0};
	struct bcm2712_dma_root_window ram[2];
	unsigned i;
	int error;

	if (d == NULL || out == NULL || d->reg == NULL || d->ranges == NULL ||
	    d->dma == NULL || d->resets == NULL || d->interrupts == NULL ||
	    d->interrupt_map == NULL || d->nreg != 4 || d->nranges != 14 ||
	    d->ndma != 21 || d->nresets != 3 || d->ninterrupts != 6 ||
	    d->ninterrupt_map != 32)
		return (EINVAL);
	if (d->unsupported != 0 || d->parent_acells != 2 || d->parent_scells != 2 ||
	    d->acells != 3 || d->scells != 2 || d->icells != 1 || d->domain != 2 ||
	    d->lanes != 4 || d->generation != 2 || d->msi_parent != 0x27 ||
	    d->interrupt_parent != 1 || d->interrupt_mask[0] != 0 ||
	    d->interrupt_mask[1] != 0 || d->interrupt_mask[2] != 0 ||
	    d->interrupt_mask[3] != 7 || memcmp(d->reg, reg, sizeof(reg)) != 0 ||
	    memcmp(d->ranges, ranges, sizeof(ranges)) != 0 ||
	    memcmp(d->dma, dma, sizeof(dma)) != 0 ||
	    memcmp(d->resets, resets, sizeof(resets)) != 0 ||
	    memcmp(d->interrupts, interrupts, sizeof(interrupts)) != 0 ||
	    memcmp(d->interrupt_map, imap, sizeof(imap)) != 0 ||
	    memcmp(d->qos, qos, sizeof(qos)) != 0)
		return (EOPNOTSUPP);
	for (i = 0; i < 2; i++) {
		stage.outbound[i] = (struct bcm2712_pcie_window){
		    pair(d->ranges + i*7 + 1), pair(d->ranges + i*7 + 3),
		    pair(d->ranges + i*7 + 5)};
		error = bcm2712_pcie_encode_outbound(&stage.outbound[i], &stage.out[i]);
		if (error != 0)
			return (error);
	}
	/* Unused windows have base > limit, not a widened resource. */
	for (i = 2; i < 4; i++) {
		stage.out[i].base_limit = 0xfff0;
		stage.out[i].base_hi = 0xff;
		stage.out[i].decode_first = CPU_LIMIT & ~(MIB - 1);
		stage.out[i].decode_last = MIB - 1;
	}
	for (i = 0; i < 3; i++) {
		stage.inbound[i] = (struct bcm2712_pcie_window){
		    pair(d->dma + i*7 + 1), pair(d->dma + i*7 + 3),
		    pair(d->dma + i*7 + 5)};
		error = bcm2712_pcie_encode_inbound(&stage.inbound[i], &stage.in[i]);
		if (error != 0)
			return (error);
	}
	for (i = 0; i < 2; i++)
		ram[i] = (struct bcm2712_dma_root_window){stage.inbound[i].cpu,
		    stage.inbound[i].pci, stage.inbound[i].length,
		    i == 0 ? BCM2712_DMA_PEER : BCM2712_DMA_RAM, d->dma[i*7]};
	error = bcm2712_dma_policy_root(&stage.ram_policy, ram, 2);
	if (error != 0)
		return (error);
	stage.raw_msi = (struct bcm2712_dma_msi_route){stage.inbound[2].pci,
	    stage.inbound[2].cpu, stage.inbound[2].length};
	for (i = 0; i < 8; i++)
		stage.qos |= (uint32_t)d->qos[i] << (i*4);
	stage.validated = PLAN_VALID;
	*out = stage;
	return (0);
}

static int
write_check(const struct bcm2712_pcie_ops *o, void *c, uint32_t reg,
    unsigned width, uint32_t value, uint32_t mask)
{
	uint32_t observed;
	int error;

	error = o->write(c, reg, width, value);
	if (error == 0)
		error = o->read(c, reg, width, &observed);
	if (error == 0 && (observed & mask) != (value & mask))
		error = EIO;
	return (error);
}

static int
update(const struct bcm2712_pcie_ops *o, void *c, uint32_t reg,
    unsigned width, uint32_t clear, uint32_t set)
{
	uint32_t value;
	int error;

	error = o->read(c, reg, width, &value);
	if (error != 0)
		return (error);
	return (write_check(o, c, reg, width, (value & ~clear) | set, clear | set));
}

/* MDIO command/data completion semantics are from the ISC OpenBSD driver.
 * No undocumented PHY-data readback is assumed for the 54MHz table. */
static int
mdio_write(const struct bcm2712_pcie_ops *o, void *c, uint16_t reg, uint16_t data)
{
	uint32_t observed;
	unsigned i;
	int error;

	error = o->write(c, 0x1100, 4, reg);
	if (error == 0)
		error = o->read(c, 0x1100, 4, &observed);
	if (error == 0 && (observed & 0x001fffff) != reg)
		error = EIO;
	if (error == 0)
		error = o->write(c, 0x1104, 4, UINT32_C(0x80000000) | data);
	if (error != 0)
		return (error);
	for (i = 0; i < 10; i++) {
		error = o->read(c, 0x1104, 4, &observed);
		if (error != 0)
			return (error);
		if ((observed & UINT32_C(0x80000000)) == 0)
			return (0);
		o->delay(c, 10);
	}
	return (ETIMEDOUT);
}

int
bcm2712_pcie_start(const struct bcm2712_pcie_plan *p,
    const struct bcm2712_pcie_ops *o, void *c, struct bcm2712_pcie_state *s)
{
	static const uint16_t mdio_regs[] = {0x1f,0x16,0x17,0x18,0x19,0x1b,0x1c,0x1e};
	static const uint16_t mdio_data[] = {0x1600,0x50b9,0xbda1,0x0094,0x97b4,0x5030,0x5030,0x0007};
	uint32_t value, reg;
	unsigned i;
	int error;

	if (p == NULL || p->validated != PLAN_VALID || s == NULL || o == NULL ||
	    o->read == NULL || o->write == NULL || o->reset == NULL ||
	    o->delay == NULL || o->dependencies == NULL || o->publish == NULL)
		return (EINVAL);
	if (s->touched || s->published)
		return (EBUSY);
#define TRY(call) do { error = (call); if (error != 0) goto fail; } while (0)
	TRY(o->dependencies(c));
	s->phase = 1;
	/* BCM2712 bridge release precedes the checked rescal pulse. */
	s->touched = 1;
	TRY(o->reset(c, BCM2712_PCIE_BRIDGE, 0));
	TRY(o->reset(c, BCM2712_PCIE_RESCAL, 1));
	TRY(o->reset(c, BCM2712_PCIE_RESCAL, 0));
	TRY(o->reset(c, BCM2712_PCIE_BRIDGE, 1));
	o->delay(c, 200);
	TRY(o->reset(c, BCM2712_PCIE_BRIDGE, 0));
	s->phase = 2;
	/* Explicit BCM2712 inverted PERST, after bridge registers are live. */
	TRY(update(o,c,0x4064,4,4,0));
	TRY(update(o,c,0x4304,4,1U<<27,0));
	o->delay(c, 200);
	TRY(o->read(c,0x4068,4,&value));
	if ((value & 0x80) == 0) { error = ENXIO; goto fail; }
	TRY(update(o,c,0x4008,4,0x300000,
	    0x200000 | (1U<<12) | (1U<<13) | 0x480));
	TRY(update(o,c,0x043c,4,0xffffff,0x060400));
	TRY(update(o,c,0x0188,4,0x0c,0));
	/* Disable the embedded MSI matcher; external raw MIP route is BAR3. */
	TRY(update(o,c,0x4044,4,1,0));
	s->phase = 3;
	for (i = 0; i < 4; i++) {
		TRY(write_check(o,c,0x400c+i*8,4,p->out[i].pci_lo,UINT32_MAX));
		TRY(write_check(o,c,0x4010+i*8,4,p->out[i].pci_hi,UINT32_MAX));
		TRY(write_check(o,c,0x4070+i*4,4,p->out[i].base_limit,0xfff0fff0));
		TRY(write_check(o,c,0x4080+i*8,4,p->out[i].base_hi,0xff));
		TRY(write_check(o,c,0x4084+i*8,4,p->out[i].limit_hi,0xff));
	}
	for (i = 0; i < 10; i++) {
		reg = i < 3 ? 0x402c + i*8 : 0x40d4 + (i-3)*8;
		TRY(write_check(o,c,reg,4,p->in[i].pci_lo,UINT32_MAX));
		TRY(write_check(o,c,reg+4,4,p->in[i].pci_hi,UINT32_MAX));
		reg = i < 3 ? 0x40ac + i*8 : 0x410c + (i-3)*8;
		TRY(write_check(o,c,reg,4,p->in[i].cpu_lo,UINT32_MAX));
		TRY(write_check(o,c,reg+4,4,p->in[i].cpu_hi,UINT32_MAX));
	}
	s->phase = 4;
	for (i = 0; i < sizeof(mdio_regs)/sizeof(mdio_regs[0]); i++)
		TRY(mdio_write(o,c,mdio_regs[i],mdio_data[i]));
	o->delay(c, 100);
	TRY(update(o,c,0x184c,4,0xff,18));
	TRY(update(o,c,0x40a4,4,0,(1U<<13)|(1U<<19)));
	TRY(write_check(o,c,0x4170,4,UINT32_MAX,UINT32_MAX));
	TRY(write_check(o,c,0x40a8,4,0x0b2d0000,UINT32_MAX));
	TRY(write_check(o,c,0x405c,4,0x0aba0000,UINT32_MAX));
	/* Bit12 is reserved-zero on affected revisions; check then apply the
	 * maintained best-effort outstanding-request fallback, not a fake fix. */
	TRY(o->read(c,0x416c,4,&value));
	value = (value & ~(1U<<17)) | (1U<<13) | (1U<<12) | (1U<<11);
	TRY(write_check(o,c,0x416c,4,value,(1U<<17)|(1U<<13)|(1U<<11)));
	TRY(o->read(c,0x416c,4,&value));
	if ((value & (1U<<12)) == 0) {
		s->qos_fallback = 1;
		TRY(update(o,c,0x416c,4,0x3f,15));
	}
	TRY(update(o,c,0x40a0,4,1U<<5,0));
	TRY(write_check(o,c,0x4168,4,p->qos,UINT32_MAX));
	TRY(write_check(o,c,0x4164,4,p->qos,UINT32_MAX));
	TRY(write_check(o,c,0x0a0c,4,0,0xffff));
	TRY(update(o,c,0x0a20,4,0,(1U<<16)|(1U<<17)|(1U<<18)));
	TRY(update(o,c,0x40a0,4,0,1U<<5));
	s->phase = 5;
	TRY(update(o,c,0x04dc,4,0xc00|0x1ff,0x800|0x42));
	TRY(update(o,c,0x00dc,2,0xf,2));
	TRY(update(o,c,0x4304,4,0x00310002,0));
	TRY(update(o,c,0x4064,4,0,4));
	o->delay(c, 100000);
	for (i = 0; i < 20; i++) {
		TRY(o->read(c,0x4068,4,&value));
		if ((value & 0x30) == 0x30)
			break;
		o->delay(c, 5000);
	}
	if (i == 20) { error = ETIMEDOUT; goto fail; }
	TRY(o->read(c,0x00be,2,&value));
	s->generation = value & 0xf;
	s->lanes = (value >> 4) & 0x3f;
	if (s->generation == 0 || s->generation > 2 || s->lanes == 0 || s->lanes > 4) {
		error = EIO; goto fail;
	}
	TRY(update(o,c,0x4304,4,0x00310002,1U<<21));
	s->link = 1;
	s->phase = 6;
	TRY(o->dependencies(c));
	s->publication_attempted = 1;
	TRY(o->publish(c));
	s->published = 1;
	s->phase = 7;
	s->error = 0;
	return (0);
fail:
	s->error = error;
	s->retained = s->touched;
	return (error);
#undef TRY
}
