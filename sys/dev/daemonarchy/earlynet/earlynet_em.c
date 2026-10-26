/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */
/*
 * Early polled backend for Intel 8254x (e1000) functions, as emulated by
 * QEMU (82540EM, 8086:100e) and bhyve (82545EM, 8086:100f).  Legacy
 * descriptors, one RX ring of EARLYNET_RX_SLOTS buffers, a TX ring used one
 * frame at a time, all interrupts masked.
 */
#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <errno.h>
#include <string.h>
#endif

#include "earlynet.h"

#define	EM_CTRL		0x0000
#define	EM_STATUS	0x0008
#define	EM_IMC		0x00d8
#define	EM_RCTL		0x0100
#define	EM_TCTL		0x0400
#define	EM_TIPG		0x0410
#define	EM_RDBAL	0x2800
#define	EM_RDBAH	0x2804
#define	EM_RDLEN	0x2808
#define	EM_RDH		0x2810
#define	EM_RDT		0x2818
#define	EM_TDBAL	0x3800
#define	EM_TDBAH	0x3804
#define	EM_TDLEN	0x3808
#define	EM_TDH		0x3810
#define	EM_TDT		0x3818
#define	EM_MTA		0x5200
#define	EM_RAL0		0x5400
#define	EM_RAH0		0x5404

#define	EM_CTRL_SLU	0x00000040
#define	EM_CTRL_RST	0x04000000
#define	EM_RCTL_EN	0x00000002
#define	EM_RCTL_BAM	0x00008000
#define	EM_RCTL_SECRC	0x04000000
#define	EM_TCTL_EN	0x00000002
#define	EM_TCTL_PSP	0x00000008
#define	EM_TCTL_CT	(0x0f << 4)
#define	EM_TCTL_COLD	(0x40 << 12)
#define	EM_RAH_AV	0x80000000u
#define	EM_TXD_EOP	0x01
#define	EM_TXD_IFCS	0x02
#define	EM_TXD_RS	0x08
#define	EM_DD		0x01
#define	EM_RXD_EOP	0x02

#define	EM_TX_SLOTS	8
#define	EM_DESC		16

static uint32_t
rd(struct earlynet *en, uint32_t reg)
{
	return (en->pf->rd32(en->pf->ctx, reg));
}

static void
wr(struct earlynet *en, uint32_t reg, uint32_t value)
{
	en->pf->wr32(en->pf->ctx, reg, value);
}

static volatile uint8_t *
desc(struct earlynet_dma *ring, unsigned i)
{
	return ((volatile uint8_t *)ring->va + i * EM_DESC);
}

static void
put_addr(volatile uint8_t *d, uint64_t pa)
{
	unsigned i;

	for (i = 0; i < 8; i++)
		d[i] = (uint8_t)(pa >> (8 * i));	/* little endian */
}

static int
em_reset(struct earlynet *en)
{
	unsigned i;

	wr(en, EM_IMC, 0xffffffff);
	wr(en, EM_RCTL, 0);
	wr(en, EM_TCTL, 0);
	en->pf->sync(en->pf->ctx);
	wr(en, EM_CTRL, rd(en, EM_CTRL) | EM_CTRL_RST);
	for (i = 0; i < EARLYNET_RESET_POLLS; i++) {
		en->pf->delay_us(en->pf->ctx, EARLYNET_POLL_US);
		if ((rd(en, EM_CTRL) & EM_CTRL_RST) == 0) {
			wr(en, EM_IMC, 0xffffffff);
			return (0);
		}
	}
	return (ETIMEDOUT);
}

static void
em_free(struct earlynet *en)
{
	struct earlynet_em *em = &en->u.em;

	if (em->bufs.va != NULL)
		en->pf->dma_free(en->pf->ctx, &em->bufs);
	if (em->tx_ring.va != NULL)
		en->pf->dma_free(en->pf->ctx, &em->tx_ring);
	if (em->rx_ring.va != NULL)
		en->pf->dma_free(en->pf->ctx, &em->rx_ring);
	memset(em, 0, sizeof(*em));
}

static int
em_attach(struct earlynet *en)
{
	struct earlynet_em *em = &en->u.em;
	uint32_t low, high;
	unsigned i;
	int error;

	memset(em, 0, sizeof(*em));
	/* The firmware or loader left the receive address; keep that MAC. */
	low = rd(en, EM_RAL0);
	high = rd(en, EM_RAH0);
	if ((high & EM_RAH_AV) == 0)
		return (ENXIO);
	for (i = 0; i < 4; i++)
		en->mac[i] = (uint8_t)(low >> (8 * i));
	en->mac[4] = (uint8_t)high;
	en->mac[5] = (uint8_t)(high >> 8);
	if ((error = em_reset(en)) != 0)
		return (error);
	if ((error = en->pf->dma_alloc(en->pf->ctx, EARLYNET_RX_SLOTS * EM_DESC, 128,
	    &em->rx_ring)) != 0 ||
	    (error = en->pf->dma_alloc(en->pf->ctx, EM_TX_SLOTS * EM_DESC, 128,
	    &em->tx_ring)) != 0 ||
	    (error = en->pf->dma_alloc(en->pf->ctx,
	    (EARLYNET_RX_SLOTS + 1) * EARLYNET_BUF_SIZE, 4096, &em->bufs)) != 0) {
		em_free(en);
		return (error);
	}
	wr(en, EM_RAL0, low);
	wr(en, EM_RAH0, high | EM_RAH_AV);
	for (i = 0; i < 128; i++)
		wr(en, EM_MTA + 4 * i, 0);
	for (i = 0; i < EARLYNET_RX_SLOTS; i++) {
		volatile uint8_t *d = desc(&em->rx_ring, i);

		memset((void *)(uintptr_t)d, 0, EM_DESC);
		put_addr(d, em->bufs.pa + (uint64_t)i * EARLYNET_BUF_SIZE);
	}
	memset(em->tx_ring.va, 0, EM_TX_SLOTS * EM_DESC);
	en->pf->sync(en->pf->ctx);
	wr(en, EM_CTRL, rd(en, EM_CTRL) | EM_CTRL_SLU);
	wr(en, EM_RDBAL, (uint32_t)em->rx_ring.pa);
	wr(en, EM_RDBAH, (uint32_t)(em->rx_ring.pa >> 32));
	wr(en, EM_RDLEN, EARLYNET_RX_SLOTS * EM_DESC);
	wr(en, EM_RDH, 0);
	wr(en, EM_RDT, EARLYNET_RX_SLOTS - 1);
	wr(en, EM_TDBAL, (uint32_t)em->tx_ring.pa);
	wr(en, EM_TDBAH, (uint32_t)(em->tx_ring.pa >> 32));
	wr(en, EM_TDLEN, EM_TX_SLOTS * EM_DESC);
	wr(en, EM_TDH, 0);
	wr(en, EM_TDT, 0);
	wr(en, EM_TIPG, 0x0060200a);
	wr(en, EM_TCTL, EM_TCTL_EN | EM_TCTL_PSP | EM_TCTL_CT | EM_TCTL_COLD);
	/* 2048-byte buffers (BSIZE 0), broadcast for ARP, strip CRC. */
	wr(en, EM_RCTL, EM_RCTL_EN | EM_RCTL_BAM | EM_RCTL_SECRC);
	return (0);
}

static int
em_send(struct earlynet *en, const uint8_t *frame, size_t len)
{
	struct earlynet_em *em = &en->u.em;
	volatile uint8_t *d = desc(&em->tx_ring, em->tx_tail);
	uint64_t pa = em->bufs.pa + (uint64_t)EARLYNET_RX_SLOTS * EARLYNET_BUF_SIZE;
	uint8_t *buf = (uint8_t *)em->bufs.va + EARLYNET_RX_SLOTS * EARLYNET_BUF_SIZE;
	unsigned i;

	if (len > EARLYNET_FRAME || em->tx_ring.va == NULL)
		return (EINVAL);
	memcpy(buf, frame, len);
	memset((void *)(uintptr_t)d, 0, EM_DESC);
	put_addr(d, pa);
	d[8] = (uint8_t)len;
	d[9] = (uint8_t)(len >> 8);
	d[11] = EM_TXD_EOP | EM_TXD_IFCS | EM_TXD_RS;
	en->pf->sync(en->pf->ctx);
	em->tx_tail = (em->tx_tail + 1) % EM_TX_SLOTS;
	wr(en, EM_TDT, em->tx_tail);
	for (i = 0; i < EARLYNET_TX_POLLS; i++) {
		en->pf->sync(en->pf->ctx);
		if (d[12] & EM_DD)
			return (0);
		en->pf->delay_us(en->pf->ctx, EARLYNET_POLL_US);
	}
	return (ETIMEDOUT);
}

static unsigned
em_poll(struct earlynet *en, unsigned budget)
{
	struct earlynet_em *em = &en->u.em;
	unsigned n;

	for (n = 0; n < budget && em->rx_ring.va != NULL; n++) {
		volatile uint8_t *d = desc(&em->rx_ring, em->rx_next);
		size_t len;

		en->pf->sync(en->pf->ctx);
		if ((d[12] & EM_DD) == 0)
			break;
		len = (size_t)d[8] | (size_t)d[9] << 8;
		if ((d[12] & EM_RXD_EOP) != 0 && d[13] == 0 && len <= EARLYNET_FRAME)
			earlynet_input(en, (const uint8_t *)em->bufs.va +
			    em->rx_next * EARLYNET_BUF_SIZE, len);
		else
			en->rx_ignored++;
		d[12] = 0;
		en->pf->sync(en->pf->ctx);
		/* Return the slot: the tail trails the next slot we read. */
		wr(en, EM_RDT, em->rx_next);
		em->rx_next = (em->rx_next + 1) % EARLYNET_RX_SLOTS;
	}
	return (n);
}

static int
em_quiesce(struct earlynet *en)
{
	int error;

	error = em_reset(en);
	if (error != 0)
		return (error);	/* DMA may continue: keep the memory */
	wr(en, EM_RDLEN, 0);
	wr(en, EM_TDLEN, 0);
	em_free(en);
	return (0);
}

const struct earlynet_backend earlynet_em_backend = {
	.name = "em",
	.attach = em_attach,
	.send = em_send,
	.poll = em_poll,
	.quiesce = em_quiesce,
};
