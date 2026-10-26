/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */
/*
 * Early polled backend for legacy (transitional) virtio-net over the PCI
 * I/O BAR, as provided by bhyve and QEMU (1af4:1000).  Only VIRTIO_NET_F_MAC
 * is negotiated, interrupts are suppressed with VRING_AVAIL_F_NO_INTERRUPT,
 * and EARLYNET_RX_SLOTS receive buffers plus one transmit buffer are used.
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

#define	VT_HOST_FEATURES	0
#define	VT_GUEST_FEATURES	4
#define	VT_QUEUE_PFN		8
#define	VT_QUEUE_NUM		12
#define	VT_QUEUE_SEL		14
#define	VT_QUEUE_NOTIFY		16
#define	VT_STATUS		18
#define	VT_ISR			19
#define	VT_CONFIG		20	/* without MSI-X */

#define	VT_ACK			0x01
#define	VT_DRIVER		0x02
#define	VT_DRIVER_OK		0x04
#define	VT_FAILED		0x80
#define	VT_F_MAC		(1u << 5)
#define	VRING_DESC_F_WRITE	2
#define	VRING_AVAIL_F_NO_INTERRUPT 1
#define	VT_HDR			10	/* legacy virtio_net_hdr, no MRG_RXBUF */
#define	VT_MAX_NUM		1024
#define	VT_PAGE			4096
#define	RXQ			0
#define	TXQ			1

static size_t
align_up(size_t n, size_t a)
{
	return ((n + a - 1) & ~(a - 1));
}

static size_t
vring_size(unsigned num)
{
	return (align_up(16 * num + 6 + 2 * num, VT_PAGE) +
	    align_up(6 + 8 * num, VT_PAGE));
}

/* Ring views; the device and this code share little-endian layouts. */
static volatile uint8_t *
vq_desc(struct earlynet_vtnet *vt, int q, unsigned i)
{
	return ((volatile uint8_t *)vt->vq[q].va + 16 * i);
}

static volatile uint8_t *
vq_avail(struct earlynet_vtnet *vt, int q)
{
	return ((volatile uint8_t *)vt->vq[q].va + 16 * vt->num[q]);
}

static volatile uint8_t *
vq_used(struct earlynet_vtnet *vt, int q)
{
	return ((volatile uint8_t *)vt->vq[q].va +
	    align_up(16 * vt->num[q] + 6 + 2 * vt->num[q], VT_PAGE));
}

static void
le16(volatile uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void
le32(volatile uint8_t *p, uint32_t v)
{
	le16(p, (uint16_t)v);
	le16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t
rle16(volatile const uint8_t *p)
{
	return ((uint16_t)(p[0] | p[1] << 8));
}

static uint32_t
rle32(volatile const uint8_t *p)
{
	return ((uint32_t)rle16(p) | (uint32_t)rle16(p + 2) << 16);
}

static void
set_desc(struct earlynet_vtnet *vt, int q, unsigned i, uint64_t pa,
    uint32_t len, uint16_t flags)
{
	volatile uint8_t *d = vq_desc(vt, q, i);

	le32(d, (uint32_t)pa);
	le32(d + 4, (uint32_t)(pa >> 32));
	le32(d + 8, len);
	le16(d + 12, flags);
	le16(d + 14, 0);
}

static void
publish(struct earlynet *en, int q, uint16_t id)
{
	struct earlynet_vtnet *vt = &en->u.vt;
	volatile uint8_t *avail = vq_avail(vt, q);

	le16(avail + 4 + 2 * (vt->avail_idx[q] % vt->num[q]), id);
	en->pf->sync(en->pf->ctx);
	vt->avail_idx[q]++;
	le16(avail + 2, vt->avail_idx[q]);
	en->pf->sync(en->pf->ctx);
	en->pf->wr16(en->pf->ctx, VT_QUEUE_NOTIFY, (uint16_t)q);
}

static void
vt_free(struct earlynet *en)
{
	struct earlynet_vtnet *vt = &en->u.vt;
	int q;

	for (q = 0; q < 2; q++)
		if (vt->vq[q].va != NULL)
			en->pf->dma_free(en->pf->ctx, &vt->vq[q]);
	if (vt->bufs.va != NULL)
		en->pf->dma_free(en->pf->ctx, &vt->bufs);
	memset(vt, 0, sizeof(*vt));
}

static int
vt_reset(struct earlynet *en)
{
	unsigned i;

	en->pf->wr8(en->pf->ctx, VT_STATUS, 0);
	/* A legacy device acknowledges reset by reading back zero. */
	for (i = 0; i < EARLYNET_RESET_POLLS; i++) {
		if (en->pf->rd8(en->pf->ctx, VT_STATUS) == 0)
			return (0);
		en->pf->delay_us(en->pf->ctx, EARLYNET_POLL_US);
	}
	return (ETIMEDOUT);
}

static int
vt_attach(struct earlynet *en)
{
	struct earlynet_vtnet *vt = &en->u.vt;
	unsigned i;
	int q, error;

	memset(vt, 0, sizeof(*vt));
	if ((error = vt_reset(en)) != 0)
		return (error);
	en->pf->wr8(en->pf->ctx, VT_STATUS, VT_ACK);
	en->pf->wr8(en->pf->ctx, VT_STATUS, VT_ACK | VT_DRIVER);
	if ((en->pf->rd32(en->pf->ctx, VT_HOST_FEATURES) & VT_F_MAC) == 0)
		goto failed;
	en->pf->wr32(en->pf->ctx, VT_GUEST_FEATURES, VT_F_MAC);
	for (i = 0; i < 6; i++)
		en->mac[i] = en->pf->rd8(en->pf->ctx, VT_CONFIG + i);
	for (q = 0; q < 2; q++) {
		en->pf->wr16(en->pf->ctx, VT_QUEUE_SEL, (uint16_t)q);
		vt->num[q] = en->pf->rd16(en->pf->ctx, VT_QUEUE_NUM);
		if (vt->num[q] < EARLYNET_RX_SLOTS || vt->num[q] > VT_MAX_NUM ||
		    (vt->num[q] & (vt->num[q] - 1)) != 0 ||
		    en->pf->rd32(en->pf->ctx, VT_QUEUE_PFN) != 0)
			goto failed;
		if (en->pf->dma_alloc(en->pf->ctx, vring_size(vt->num[q]), VT_PAGE,
		    &vt->vq[q]) != 0)
			goto failed;
		/* A legacy queue address is a 32-bit page frame number. */
		if (vt->vq[q].pa / VT_PAGE > UINT32_MAX)
			goto failed;
		memset(vt->vq[q].va, 0, vring_size(vt->num[q]));
		le16(vq_avail(vt, q), VRING_AVAIL_F_NO_INTERRUPT);
		en->pf->sync(en->pf->ctx);
		en->pf->wr32(en->pf->ctx, VT_QUEUE_PFN, (uint32_t)(vt->vq[q].pa / VT_PAGE));
	}
	if (en->pf->dma_alloc(en->pf->ctx, (EARLYNET_RX_SLOTS + 1) * EARLYNET_BUF_SIZE,
	    VT_PAGE, &vt->bufs) != 0)
		goto failed;
	for (i = 0; i < EARLYNET_RX_SLOTS; i++)
		set_desc(vt, RXQ, i, vt->bufs.pa + (uint64_t)i * EARLYNET_BUF_SIZE,
		    VT_HDR + EARLYNET_FRAME, VRING_DESC_F_WRITE);
	en->pf->wr8(en->pf->ctx, VT_STATUS, VT_ACK | VT_DRIVER | VT_DRIVER_OK);
	for (i = 0; i < EARLYNET_RX_SLOTS; i++)
		publish(en, RXQ, (uint16_t)i);
	return (0);
failed:
	en->pf->wr8(en->pf->ctx, VT_STATUS, VT_FAILED);
	(void)vt_reset(en);
	vt_free(en);
	return (ENXIO);
}

static int
vt_send(struct earlynet *en, const uint8_t *frame, size_t len)
{
	struct earlynet_vtnet *vt = &en->u.vt;
	uint8_t *buf;
	unsigned i;

	if (len > EARLYNET_FRAME || vt->bufs.va == NULL)
		return (EINVAL);
	buf = (uint8_t *)vt->bufs.va + EARLYNET_RX_SLOTS * EARLYNET_BUF_SIZE;
	memset(buf, 0, VT_HDR);
	memcpy(buf + VT_HDR, frame, len);
	set_desc(vt, TXQ, 0, vt->bufs.pa + (uint64_t)EARLYNET_RX_SLOTS *
	    EARLYNET_BUF_SIZE, (uint32_t)(VT_HDR + len), 0);
	publish(en, TXQ, 0);
	for (i = 0; i < EARLYNET_TX_POLLS; i++) {
		en->pf->sync(en->pf->ctx);
		if (rle16(vq_used(vt, TXQ) + 2) != vt->used_seen[TXQ]) {
			vt->used_seen[TXQ]++;
			return (0);
		}
		en->pf->delay_us(en->pf->ctx, EARLYNET_POLL_US);
	}
	return (ETIMEDOUT);
}

static unsigned
vt_poll(struct earlynet *en, unsigned budget)
{
	struct earlynet_vtnet *vt = &en->u.vt;
	volatile uint8_t *used;
	unsigned n;

	if (vt->bufs.va == NULL)
		return (0);
	(void)en->pf->rd8(en->pf->ctx, VT_ISR);	/* clear a pending INTx */
	used = vq_used(vt, RXQ);
	for (n = 0; n < budget; n++) {
		volatile uint8_t *elem;
		uint32_t id, len;

		en->pf->sync(en->pf->ctx);
		if (rle16(used + 2) == vt->used_seen[RXQ])
			break;
		elem = used + 4 + 8 * (vt->used_seen[RXQ] % vt->num[RXQ]);
		id = rle32(elem);
		len = rle32(elem + 4);
		vt->used_seen[RXQ]++;
		if (id >= EARLYNET_RX_SLOTS) {
			en->rx_ignored++;
			continue;
		}
		if (len > VT_HDR && len <= VT_HDR + EARLYNET_FRAME)
			earlynet_input(en, (const uint8_t *)vt->bufs.va +
			    id * EARLYNET_BUF_SIZE + VT_HDR, len - VT_HDR);
		else
			en->rx_ignored++;
		publish(en, RXQ, (uint16_t)id);
	}
	return (n);
}

static int
vt_quiesce(struct earlynet *en)
{
	int q, error;

	/* Reset stops the device from using the rings; then forget them. */
	if ((error = vt_reset(en)) != 0)
		return (error);
	for (q = 0; q < 2; q++) {
		en->pf->wr16(en->pf->ctx, VT_QUEUE_SEL, (uint16_t)q);
		en->pf->wr32(en->pf->ctx, VT_QUEUE_PFN, 0);
	}
	vt_free(en);
	return (0);
}

const struct earlynet_backend earlynet_vtnet_backend = {
	.name = "vtnet",
	.attach = vt_attach,
	.send = vt_send,
	.poll = vt_poll,
	.quiesce = vt_quiesce,
};
