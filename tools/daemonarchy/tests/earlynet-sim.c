/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Early transport checks: the production earlynet core and its em and
 * vtnet backends, compiled unchanged, against simulated e1000 and legacy
 * virtio-net devices and a scripted debugnet controller on the wire.
 * Device DMA goes through a physical-address map, so an access to
 * released memory aborts the case.
 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error "Early transport checks require assertions"
#endif

#include "sys/dev/daemonarchy/earlynet/earlynet.h"

/* ---------- platform ---------- */

static int live_dma, delays;
static long delay_us_total;
enum { DEV_EM, DEV_VT } dev;

/* Fake physical addresses below 4 GiB, mapped to host allocations. */
static struct { uint64_t pa; void *va; size_t size; } maps[16];
static uint64_t next_pa = 0x100000;

static int
dma_alloc(void *ctx, size_t size, size_t align, struct earlynet_dma *d)
{
	(void)ctx;
	d->va = aligned_alloc(align, (size + align - 1) / align * align);
	if (d->va == NULL)
		return (ENOMEM);
	memset(d->va, 0xa5, size);	/* the backend must initialise rings */
	next_pa = (next_pa + align - 1) / align * align;
	d->pa = next_pa;
	next_pa += size;
	d->size = size;
	for (int i = 0; i < 16; i++)
		if (maps[i].va == NULL) {
			maps[i].pa = d->pa; maps[i].va = d->va; maps[i].size = size;
			live_dma++;
			return (0);
		}
	abort();
}

static void
dma_free(void *ctx, struct earlynet_dma *d)
{
	(void)ctx;
	for (int i = 0; i < 16; i++)
		if (maps[i].va == d->va) {
			maps[i].va = NULL;
			free(d->va);
			d->va = NULL;
			live_dma--;
			return;
		}
	abort();
}

static void
delay_us(void *ctx, unsigned us)
{
	(void)ctx;
	delays++;
	delay_us_total += us;
}

/* A barrier is also where the simulated device performs pending DMA. */
static void device_pump(void);
static void sync_mem(void *ctx) { (void)ctx; __sync_synchronize(); device_pump(); }

/* Device view of a physical address; a released or unknown one aborts. */
static void *
mem(uint64_t pa)
{
	for (int i = 0; i < 16; i++)
		if (maps[i].va != NULL && pa >= maps[i].pa && pa < maps[i].pa + maps[i].size)
			return ((char *)maps[i].va + (pa - maps[i].pa));
	fprintf(stderr, "device DMA to unmapped physical address %#llx\n",
	    (unsigned long long)pa);
	abort();
}

/* ---------- wire and scripted controller ---------- */

#define	MAXF 64
static uint8_t rxq[MAXF][1600];
static size_t rxlen[MAXF];
static int rxh, rxc;
static uint8_t peer_mac[6] = { 2, 0, 0, 0, 0, 1 };
static uint8_t local_ip[4] = { 192, 0, 2, 2 }, peer_ip[4] = { 192, 0, 2, 1 };
static struct {
	int reply_arp, ack_data, ack_herald;
	int arp_requests, heralds, data, finished, acks_from_target, frames;
	char herald[64];
	uint8_t output[4096];
	size_t outlen;
	uint32_t last_ack_from_target;
} ctl;

static void
inject(const uint8_t *f, size_t len)
{
	assert(rxc < MAXF);
	memcpy(rxq[(rxh + rxc) % MAXF], f, len);
	rxlen[(rxh + rxc++) % MAXF] = len;
}

static void
p16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void
p32(uint8_t *p, uint32_t v) { p16(p, v >> 16); p16(p + 2, v); }
static uint32_t
g32(const uint8_t *p) { return ((uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]); }

static uint16_t
cksum(const uint8_t *p, size_t n)
{
	uint32_t s = 0;
	for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return ((uint16_t)~s);
}

static const uint8_t *target_mac;

/* Build a controller-to-target UDP frame. */
static size_t
udp_frame(uint8_t *f, const uint8_t *payload, size_t len, uint16_t sport)
{
	uint8_t *ip = f + 14, *udp = ip + 20;
	memcpy(f, target_mac, 6);
	memcpy(f + 6, peer_mac, 6);
	p16(f + 12, 0x0800);
	memset(ip, 0, 20);
	ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
	p16(ip + 2, (uint16_t)(28 + len));
	memcpy(ip + 12, peer_ip, 4);
	memcpy(ip + 16, local_ip, 4);
	p16(ip + 10, cksum(ip, 20));
	p16(udp, sport); p16(udp + 2, 26004); p16(udp + 4, (uint16_t)(8 + len)); p16(udp + 6, 0);
	memcpy(udp + 8, payload, len);
	return (42 + len < 60 ? 60 : 42 + len);
}

static void
ctl_ack(uint32_t seq)
{
	uint8_t f[128], a[4];
	p32(a, seq);
	inject(f, udp_frame(f, a, 4, 26003));
}

static void
ctl_input(uint32_t outer, uint32_t inner, const char *text)
{
	uint8_t f[1600], m[1024];
	size_t n = strlen(text);
	memset(m, 0, 24);
	p32(m, 3); p32(m + 4, outer); p32(m + 16, (uint32_t)(4 + n));
	p32(m + 24, inner);
	memcpy(m + 28, text, n);
	inject(f, udp_frame(f, m, 28 + n, 26003));
}

/* Called for every frame the simulated device transmits. */
static void
wire(const uint8_t *f, size_t len)
{
	ctl.frames++;
	assert(len >= 42 && len <= 1514);
	assert(memcmp(f + 6, target_mac, 6) == 0);
	if (f[12] == 0x08 && f[13] == 0x06) {
		if (f[21] == 1 && memcmp(f + 38, peer_ip, 4) == 0) {
			ctl.arp_requests++;
			if (ctl.reply_arp) {
				uint8_t r[60] = { 0 };
				memcpy(r, target_mac, 6); memcpy(r + 6, peer_mac, 6);
				p16(r + 12, 0x0806); p16(r + 14, 1); p16(r + 16, 0x0800);
				r[18] = 6; r[19] = 4; p16(r + 20, 2);
				memcpy(r + 22, peer_mac, 6); memcpy(r + 28, peer_ip, 4);
				memcpy(r + 32, target_mac, 6); memcpy(r + 38, local_ip, 4);
				inject(r, 60);
			}
		}
		return;
	}
	assert(f[12] == 0x08 && f[13] == 0x00);
	const uint8_t *ip = f + 14, *udp = ip + 20, *m = udp + 8;
	assert(cksum(ip, 20) == 0);
	assert(memcmp(f, peer_mac, 6) == 0);
	assert(udp[0] == 0x65 && udp[1] == 0x94 && udp[2] == 0x65 && udp[3] == 0x93);
	size_t ulen = (size_t)(udp[4] << 8 | udp[5]) - 8;
	if (ulen == 4) {
		ctl.acks_from_target++;
		ctl.last_ack_from_target = g32(m);
		return;
	}
	uint32_t type = g32(m), seq = g32(m + 4), n = g32(m + 16);
	assert(n == ulen - 24);
	if (type == 1) {
		ctl.heralds++;
		memcpy(ctl.herald, m + 24, n);
		ctl.herald[n] = 0;
		if (ctl.ack_herald) ctl_ack(seq);
	} else if (type == 3) {
		ctl.data++;
		if (ctl.ack_data) {
			memcpy(ctl.output + ctl.outlen, m + 24, n);
			ctl.outlen += n;
			ctl_ack(seq);
		}
	} else if (type == 2) {
		ctl.finished++;
		ctl_ack(seq);
	}
}

/* ---------- simulated e1000 ---------- */

static uint32_t emr[0x6000 / 4];
static int em_stuck_reset, em_reset_count;
#define	R(o) emr[(o) / 4]

static void
em_tx(void)
{
	uint8_t *ring = mem((uint64_t)R(0x3804) << 32 | R(0x3800));
	unsigned n = R(0x3808) / 16;
	while (R(0x3810) != R(0x3818)) {
		uint8_t *d = ring + 16 * R(0x3810);
		uint64_t pa = 0;
		for (int i = 7; i >= 0; i--) pa = pa << 8 | d[i];
		assert(R(0x0400) & 2);	/* transmit enabled */
		wire(mem(pa), (size_t)(d[8] | d[9] << 8));
		d[12] |= 1;
		R(0x3810) = (R(0x3810) + 1) % n;
	}
}

static void
em_rx(void)
{
	while (rxc > 0) {
		if ((R(0x0100) & 2) == 0 || R(0x2808) == 0) {	/* disabled: drop */
			rxh = (rxh + 1) % MAXF; rxc--;
			continue;
		}
		if (R(0x2810) == R(0x2818))
			return;	/* no free descriptor */
		uint8_t *ring = mem((uint64_t)R(0x2804) << 32 | R(0x2800));
		uint8_t *d = ring + 16 * R(0x2810);
		uint64_t pa = 0;
		for (int i = 7; i >= 0; i--) pa = pa << 8 | d[i];
		memcpy(mem(pa), rxq[rxh], rxlen[rxh]);
		d[8] = (uint8_t)rxlen[rxh]; d[9] = (uint8_t)(rxlen[rxh] >> 8);
		d[12] = 0x03; d[13] = 0;
		R(0x2810) = (R(0x2810) + 1) % (R(0x2808) / 16);
		rxh = (rxh + 1) % MAXF; rxc--;
	}
}

static uint32_t
em_rd32(void *ctx, uint32_t off)
{
	(void)ctx;
	em_rx();
	if (off == 0 && (R(0) & 0x04000000) && !em_stuck_reset) {
		/* reset completes: rings and enables clear, receive address kept */
		uint32_t ral = R(0x5400), rah = R(0x5404);
		memset(emr, 0, sizeof(emr));
		R(0x5400) = ral; R(0x5404) = rah;
		em_reset_count++;
	}
	return (emr[off / 4]);
}

static void
em_wr32(void *ctx, uint32_t off, uint32_t v)
{
	(void)ctx;
	emr[off / 4] = v;
	if (off == 0x3818)
		em_tx();
	em_rx();
}

static const struct earlynet_platform em_pf = {
	.rd32 = em_rd32, .wr32 = em_wr32, .dma_alloc = dma_alloc,
	.dma_free = dma_free, .delay_us = delay_us, .sync = sync_mem,
};

/* ---------- simulated legacy virtio-net ---------- */

static struct {
	uint32_t host_features, guest_features, pfn[2];
	uint16_t num, sel, last_avail[2], used_idx[2];
	uint8_t status, mac[6];
} vt;

static uint8_t *
vring(int q) { return (mem((uint64_t)vt.pfn[q] * 4096)); }
static uint16_t
l16(const uint8_t *p) { return ((uint16_t)(p[0] | p[1] << 8)); }
static uint32_t
l32(const uint8_t *p) { return ((uint32_t)l16(p) | (uint32_t)l16(p + 2) << 16); }

static uint8_t *
vt_avail(int q) { return (vring(q) + 16 * vt.num); }
static uint8_t *
vt_used(int q)
{
	size_t a = 16 * vt.num + 6 + 2 * vt.num;
	return (vring(q) + ((a + 4095) & ~(size_t)4095));
}

static void
vt_put_used(int q, uint16_t id, uint32_t len)
{
	uint8_t *u = vt_used(q), *e = u + 4 + 8 * (vt.used_idx[q] % vt.num);
	e[0] = id; e[1] = id >> 8; e[2] = e[3] = 0;
	e[4] = len; e[5] = len >> 8; e[6] = e[7] = 0;
	vt.used_idx[q]++;
	u[2] = (uint8_t)vt.used_idx[q]; u[3] = vt.used_idx[q] >> 8;
}

static void
vt_rx(void)
{
	while (rxc > 0) {
		if ((vt.status & 4) == 0 || vt.pfn[0] == 0) {
			rxh = (rxh + 1) % MAXF; rxc--;
			continue;
		}
		uint8_t *av = vt_avail(0);
		assert(l16(av) & 1);	/* interrupts suppressed */
		if (vt.last_avail[0] == l16(av + 2))
			return;
		uint16_t id = l16(av + 4 + 2 * (vt.last_avail[0]++ % vt.num));
		uint8_t *d = vring(0) + 16 * id;
		assert(l16(d + 12) & 2);	/* device-writable */
		assert(l32(d + 8) >= 10 + rxlen[rxh]);
		uint8_t *buf = mem((uint64_t)l32(d + 4) << 32 | l32(d));
		memset(buf, 0, 10);
		memcpy(buf + 10, rxq[rxh], rxlen[rxh]);
		vt_put_used(0, id, (uint32_t)(10 + rxlen[rxh]));
		rxh = (rxh + 1) % MAXF; rxc--;
	}
}

static void
vt_notify(uint16_t q)
{
	assert(vt.status & 4);
	if (q != 1) { vt_rx(); return; }
	uint8_t *av = vt_avail(1);
	while (vt.last_avail[1] != l16(av + 2)) {
		uint16_t id = l16(av + 4 + 2 * (vt.last_avail[1]++ % vt.num));
		uint8_t *d = vring(1) + 16 * id;
		uint8_t *buf = mem((uint64_t)l32(d + 4) << 32 | l32(d));
		wire(buf + 10, l32(d + 8) - 10);
		vt_put_used(1, id, 0);
	}
}

static uint8_t
vt_rd8(void *ctx, uint32_t off)
{
	(void)ctx;
	vt_rx();
	if (off == 18) return (vt.status);
	if (off == 19) return (0);
	if (off >= 20 && off < 26) return (vt.mac[off - 20]);
	abort();
}

static uint16_t
vt_rd16(void *ctx, uint32_t off) { (void)ctx; assert(off == 12); return (vt.num); }

static uint32_t
vt_rd32(void *ctx, uint32_t off)
{
	(void)ctx;
	if (off == 0) return (vt.host_features);
	if (off == 8) return (vt.pfn[vt.sel]);
	abort();
}

static void
vt_wr8(void *ctx, uint32_t off, uint8_t v)
{
	(void)ctx;
	assert(off == 18);
	vt.status = v;
	if (v == 0) {	/* reset: the device forgets its rings */
		vt.pfn[0] = vt.pfn[1] = 0;
		memset(vt.last_avail, 0, sizeof(vt.last_avail));
		memset(vt.used_idx, 0, sizeof(vt.used_idx));
	}
}

static void
vt_wr16(void *ctx, uint32_t off, uint16_t v)
{
	(void)ctx;
	if (off == 14) { assert(v < 2); vt.sel = v; }
	else if (off == 16) vt_notify(v);
	else abort();
}

static void
vt_wr32(void *ctx, uint32_t off, uint32_t v)
{
	(void)ctx;
	if (off == 4) vt.guest_features = v;
	else if (off == 8) vt.pfn[vt.sel] = v;
	else abort();
}

static const struct earlynet_platform vt_pf = {
	.rd32 = vt_rd32, .wr32 = vt_wr32, .rd16 = vt_rd16, .wr16 = vt_wr16,
	.rd8 = vt_rd8, .wr8 = vt_wr8, .dma_alloc = dma_alloc,
	.dma_free = dma_free, .delay_us = delay_us, .sync = sync_mem,
};

static void
device_pump(void)
{
	if (dev == DEV_EM)
		em_rx();
	else
		vt_rx();
}

/* ---------- harness ---------- */

static const uint8_t em_mac[6] = { 0x58, 0x9c, 0xfc, 0, 0, 0x0e };
static struct earlynet en;

static void
setup(int which)
{
	memset(&en, 0, sizeof(en));
	memset(&ctl, 0, sizeof(ctl));
	rxh = rxc = 0; live_dma = 0; delays = 0; delay_us_total = 0;
	ctl.reply_arp = ctl.ack_data = ctl.ack_herald = 1;
	dev = which;
	if (which == DEV_EM) {
		memset(emr, 0, sizeof(emr));
		em_stuck_reset = em_reset_count = 0;
		R(0x5400) = em_mac[0] | em_mac[1] << 8 | em_mac[2] << 16 | (uint32_t)em_mac[3] << 24;
		R(0x5404) = em_mac[4] | em_mac[5] << 8 | 0x80000000u;
		en.pf = &em_pf;
		en.be = earlynet_backend_for(0x8086, 0x100f);
		target_mac = em_mac;
	} else {
		memset(&vt, 0, sizeof(vt));
		vt.host_features = 1u << 5 | 1u << 16;
		vt.num = 256;
		memcpy(vt.mac, "\x58\x9c\xfc\x00\x00\x10", 6);
		en.pf = &vt_pf;
		en.be = earlynet_backend_for(0x1af4, 0x1000);
		target_mac = vt.mac;
	}
	memcpy(&en.local_ip, local_ip, 4);
	memcpy(&en.peer_ip, peer_ip, 4);
	strcpy(en.boot_id, "f14db0a0202610260000000000000003");
}

static void
started(int which)
{
	setup(which);
	assert(earlynet_start(&en) == 0);
	assert(en.owner == EARLYNET_OWNED && en.admitted);
	assert(memcmp(en.mac, target_mac, 6) == 0);
	assert(ctl.arp_requests >= 1 && ctl.heralds == 1);
	assert(strcmp(ctl.herald, en.boot_id) == 0);
	assert(!earlynet_driver_may_attach(&en));
}

static void
roundtrip(int which)
{
	started(which);
	for (const char *p = "\nmountroot> "; *p; p++) earlynet_putc(&en, *p);
	assert(earlynet_getc(&en) == -1);	/* flushes, then nothing to read */
	assert(ctl.outlen == 12 && memcmp(ctl.output, "\nmountroot> ", 12) == 0);
	ctl_input(1, 1, "ufs:/dev/vtbd0p2\n");
	char line[32] = { 0 };
	for (int i = 0, c; i < 17; i++) {
		while ((c = earlynet_getc(&en)) == -1) ;
		line[i] = (char)c;
	}
	assert(strcmp(line, "ufs:/dev/vtbd0p2\n") == 0);
	assert(ctl.last_ack_from_target == 1);
	/* A retransmission after a lost ACK is ACKed again, not delivered. */
	ctl_input(1, 1, "ufs:/dev/vtbd0p2\n");
	assert(earlynet_getc(&en) == -1);
	assert(ctl.acks_from_target == 2 && ctl.last_ack_from_target == 1);
	/* More than the free input space is refused without ACK. */
	char big[513];
	memset(big, 'x', 512); big[512] = 0;
	ctl_input(2, 2, big);
	ctl_input(3, 3, big);	/* 512 bytes fit, the second does not */
	(void)earlynet_getc(&en);
	assert(en.input_refused >= 1);
}

static void
em_roundtrip(void) { roundtrip(DEV_EM); assert((R(0x0100) & 2) && (R(0x0400) & 2)); }
static void
vt_roundtrip(void) { roundtrip(DEV_VT); assert(vt.guest_features == 1u << 5); }

static void
lost_ack_is_bounded(void)
{
	started(DEV_EM);
	ctl.ack_data = 0;
	uint32_t seq = en.tx_seq;
	int before = ctl.data;
	earlynet_putc(&en, 'x');
	assert(earlynet_flush(&en) == ETIMEDOUT);
	assert(ctl.data - before == EARLYNET_RETRIES);
	assert(en.tx_seq == seq && en.send_timeouts == 1 && en.lost_output == 1);
	assert(delay_us_total <= (long)EARLYNET_RETRIES * EARLYNET_ACK_POLLS *
	    EARLYNET_POLL_US + 1000000);
}

static void
degraded_line_flush(void)
{
	started(DEV_EM);
	for (const char *p = "boot progress\n"; *p; p++) earlynet_putc(&en, *p);
	assert(earlynet_flush_line(&en) == 0 && ctl.outlen == 14);
	ctl.ack_data = 0;
	earlynet_putc(&en, 'a');
	assert(earlynet_flush_line(&en) == ETIMEDOUT && en.degraded);
	int frames = ctl.frames;
	earlynet_putc(&en, 'b');
	assert(earlynet_flush_line(&en) == 0 && ctl.frames == frames && en.oc == 1);
	/* Hearing from the controller again re-enables line flushing. */
	ctl.ack_data = 1;
	ctl_input(1, 1, "\n");
	assert(earlynet_getc(&en) == '\n' && !en.degraded);
	assert(ctl.outlen == 15 && ctl.output[14] == 'b');
}

static void
no_controller_releases(void)
{
	setup(DEV_EM);
	ctl.reply_arp = 0;
	assert(earlynet_start(&en) == ETIMEDOUT);
	assert(ctl.arp_requests == EARLYNET_RETRIES);
	assert(en.owner == EARLYNET_RELEASED && live_dma == 0);
	assert(earlynet_driver_may_attach(&en));
	setup(DEV_VT);
	ctl.ack_herald = 0;
	assert(earlynet_start(&en) == ETIMEDOUT);
	assert(ctl.heralds == EARLYNET_RETRIES);
	assert(en.owner == EARLYNET_RELEASED && live_dma == 0 && vt.status == 0);
}

static void
handoff(int which)
{
	started(which);
	earlynet_putc(&en, '!');
	assert(earlynet_finish(&en) == 0);
	assert(ctl.finished == 1 && ctl.outlen == 1);
	assert(earlynet_quiesce(&en) == 0);
	assert(en.owner == EARLYNET_RELEASED && live_dma == 0);
	assert(earlynet_driver_may_attach(&en));
	/* After release the device must not touch the freed rings (ASan). */
	ctl_input(9, 9, "late\n");
	if (which == DEV_EM) {
		assert(R(0x0100) == 0 && R(0x0400) == 0 && R(0x2808) == 0);
		(void)em_rd32(NULL, 8);
	} else {
		assert(vt.status == 0 && vt.pfn[0] == 0 && vt.pfn[1] == 0);
		(void)vt_rd8(NULL, 18);
	}
	assert(rxc == 0);
	earlynet_putc(&en, 'y');
	assert(earlynet_getc(&en) == -1 && en.oc == 0);
}

static void em_handoff(void) { handoff(DEV_EM); assert(em_reset_count >= 2); }
static void vt_handoff(void) { handoff(DEV_VT); }

static void
failed_quiesce_keeps_ownership(void)
{
	started(DEV_EM);
	em_stuck_reset = 1;
	R(0) |= 0x04000000;	/* the reset never completes */
	assert(earlynet_quiesce(&en) == EIO);
	assert(en.owner == EARLYNET_FAILED && !earlynet_driver_may_attach(&en));
	assert(live_dma == 3);	/* DMA may continue: memory is retained */
	/* The fixture, not earlynet, may now release the simulated memory. */
	dma_free(NULL, &en.u.em.bufs);
	dma_free(NULL, &en.u.em.tx_ring);
	dma_free(NULL, &en.u.em.rx_ring);
}

static void
malformed_frames_ignored(void)
{
	started(DEV_EM);
	uint8_t f[1600];
	uint64_t before = en.rx_ignored;
	size_t n = udp_frame(f, (const uint8_t *)"abcd-not-a-message-xxxxxxx", 26, 26003);
	f[14 + 10] ^= 0xff;	/* bad IP checksum */
	inject(f, n);
	n = udp_frame(f, (const uint8_t *)"\0\0\0\x03", 4, 9999);	/* wrong port */
	inject(f, n);
	inject(f, 10);	/* runt */
	n = udp_frame(f, (const uint8_t *)"0123456789012345678901234", 25, 26003);
	inject(f, n);	/* header length disagrees with the datagram */
	uint8_t m[28] = { 0 };
	p32(m, 7); p32(m + 16, 4);
	inject(f, udp_frame(f, m, 28, 26003));	/* unknown type */
	(void)earlynet_getc(&en);
	assert(en.rx_ignored - before == 5);
	assert(en.ic == 0 && en.owner == EARLYNET_OWNED);
}

static void
vt_bad_queue(void)
{
	setup(DEV_VT);
	vt.num = 100;	/* not a power of two */
	assert(earlynet_start(&en) == ENXIO);
	assert(live_dma == 0 && en.owner == EARLYNET_RELEASED);
	setup(DEV_VT);
	vt.host_features = 0;	/* no MAC feature */
	assert(earlynet_start(&en) == ENXIO && live_dma == 0);
	assert(earlynet_backend_for(0x10ec, 0x8168) == NULL);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } cases[] = {
		{ "em-roundtrip", em_roundtrip },
		{ "vtnet-roundtrip", vt_roundtrip },
		{ "lost-ack-bounded", lost_ack_is_bounded },
		{ "degraded-line-flush", degraded_line_flush },
		{ "no-controller", no_controller_releases },
		{ "em-handoff", em_handoff },
		{ "vtnet-handoff", vt_handoff },
		{ "failed-quiesce", failed_quiesce_keeps_ownership },
		{ "malformed", malformed_frames_ignored },
		{ "vtnet-bad-queue", vt_bad_queue },
	};
	if (argc != 2)
		return (2);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
		if (strcmp(argv[1], cases[i].name) == 0) {
			cases[i].run();
			printf("%s: ok\n", argv[1]);
			return (0);
		}
	return (2);
}
