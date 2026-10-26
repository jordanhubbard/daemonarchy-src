/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */
/*
 * Protocol core of the early polled network console: ARP, IPv4/UDP and
 * debugnet-compatible framing over a backend that owns the NIC directly.
 * Everything here is bounded and polled; nothing sleeps, allocates or
 * takes locks, so it can run before interrupts, in a critical section or
 * in the debugger.
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

#define	ETHERTYPE_IP	0x0800
#define	ETHERTYPE_ARP	0x0806
#define	DN_HERALD	1
#define	DN_FINISHED	2
#define	DN_DATA		3
#define	DN_HDR		24	/* type, seq, offset(8), len, aux2 */
#define	HDRS		(14 + 20 + 8)

static void
put16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint16_t
get16(const uint8_t *p)
{
	return ((uint16_t)(p[0] << 8 | p[1]));
}

static uint32_t
get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | p[3]);
}

static uint16_t
ip_checksum(const uint8_t *p, size_t len)
{
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i + 1 < len; i += 2)
		sum += get16(p + i);
	if (len & 1)
		sum += (uint32_t)p[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ((uint16_t)~sum);
}

static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static int
send_arp(struct earlynet *en, uint16_t op, const uint8_t *dst_mac,
    uint32_t dst_ip)
{
	uint8_t *f = en->frame;

	memset(f, 0, 60);
	memcpy(f, op == 1 ? broadcast : dst_mac, 6);
	memcpy(f + 6, en->mac, 6);
	put16(f + 12, ETHERTYPE_ARP);
	put16(f + 14, 1);
	put16(f + 16, ETHERTYPE_IP);
	f[18] = 6;
	f[19] = 4;
	put16(f + 20, op);
	memcpy(f + 22, en->mac, 6);
	memcpy(f + 28, &en->local_ip, 4);
	if (op == 2)
		memcpy(f + 32, dst_mac, 6);
	memcpy(f + 38, &dst_ip, 4);
	return (en->be->send(en, f, 60));
}

/* Send one UDP datagram to the controller. */
static int
send_udp(struct earlynet *en, const uint8_t *payload, size_t len)
{
	uint8_t *f = en->frame, *ip = f + 14, *udp = ip + 20;
	size_t total = HDRS + len;
	int error;

	if (len > EARLYNET_FRAME - HDRS || !en->have_peer_mac)
		return (EINVAL);
	memcpy(f, en->peer_mac, 6);
	memcpy(f + 6, en->mac, 6);
	put16(f + 12, ETHERTYPE_IP);
	memset(ip, 0, 20);
	ip[0] = 0x45;
	put16(ip + 2, (uint16_t)(20 + 8 + len));
	ip[6] = 0x40;	/* don't fragment */
	ip[8] = 64;
	ip[9] = 17;
	memcpy(ip + 12, &en->local_ip, 4);
	memcpy(ip + 16, &en->peer_ip, 4);
	put16(ip + 10, ip_checksum(ip, 20));
	put16(udp, EARLYNET_CLIENT_PORT);
	put16(udp + 2, en->server_port);
	put16(udp + 4, (uint16_t)(8 + len));
	put16(udp + 6, 0);	/* IPv4 permits a zero UDP checksum */
	memcpy(udp + 8, payload, len);
	error = en->be->send(en, f, total < 60 ? 60 : total);
	if (error == 0)
		en->tx_frames++;
	else
		en->tx_failed++;
	return (error);
}

static void
send_ack(struct earlynet *en, uint32_t seq)
{
	uint8_t ack[4];

	put32(ack, seq);
	(void)send_udp(en, ack, sizeof(ack));
}

static void
poll_once(struct earlynet *en)
{
	(void)en->be->poll(en, EARLYNET_RX_BUDGET);
}

/* Receive path: called by backends for each received frame. */
void
earlynet_input(struct earlynet *en, const uint8_t *f, size_t len)
{
	const uint8_t *ip, *udp, *msg;
	size_t ihl, ulen, i;
	uint32_t type, seq, mlen, inner;

	en->rx_frames++;
	if (len < 14)
		goto ignore;
	if (get16(f + 12) == ETHERTYPE_ARP && len >= 42) {
		if (get16(f + 14) != 1 || get16(f + 16) != ETHERTYPE_IP ||
		    f[18] != 6 || f[19] != 4 || memcmp(f + 38, &en->local_ip, 4) != 0)
			goto ignore;
		if (memcmp(f + 28, &en->peer_ip, 4) == 0) {
			memcpy(en->peer_mac, f + 22, 6);
			en->have_peer_mac = true;
		}
		if (get16(f + 20) == 1) {
			uint32_t sender;

			memcpy(&sender, f + 28, 4);
			(void)send_arp(en, 2, f + 22, sender);
		}
		return;
	}
	if (get16(f + 12) != ETHERTYPE_IP || len < HDRS)
		goto ignore;
	ip = f + 14;
	ihl = (size_t)(ip[0] & 0x0f) * 4;
	if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17 ||
	    (get16(ip + 6) & 0x3fff) != 0 || get16(ip + 2) > len - 14 ||
	    get16(ip + 2) < ihl + 8 || ip_checksum(ip, ihl) != 0 ||
	    memcmp(ip + 12, &en->peer_ip, 4) != 0 ||
	    memcmp(ip + 16, &en->local_ip, 4) != 0)
		goto ignore;
	udp = ip + ihl;
	ulen = get16(udp + 4);
	if (get16(udp + 2) != EARLYNET_CLIENT_PORT ||
	    get16(udp) != en->server_port || ulen < 8 ||
	    ulen > get16(ip + 2) - ihl)
		goto ignore;
	msg = udp + 8;
	ulen -= 8;
	if (ulen == 4) {
		en->acked = get32(msg);
		en->degraded = false;
		return;
	}
	if (ulen < DN_HDR)
		goto ignore;
	type = get32(msg);
	seq = get32(msg + 4);
	mlen = get32(msg + 16);
	if (mlen != ulen - DN_HDR)
		goto ignore;
	if (type == DN_FINISHED) {
		en->admitted = false;
		send_ack(en, seq);
		return;
	}
	if (type != DN_DATA || mlen < 4 || mlen - 4 > EARLYNET_MAX_PAYLOAD)
		goto ignore;
	en->degraded = false;	/* the controller is reachable again */
	inner = get32(msg + DN_HDR);
	if (inner <= en->input_last) {
		send_ack(en, seq);	/* retransmission: our ACK was lost */
		return;
	}
	if (mlen - 4 > EARLYNET_INPUT - en->ic) {
		en->input_refused++;	/* no ACK: the controller retransmits */
		return;
	}
	for (i = 0; i < mlen - 4; i++)
		en->in[(en->ih + en->ic++) % EARLYNET_INPUT] = msg[DN_HDR + 4 + i];
	en->input_last = inner;
	send_ack(en, seq);
	return;
ignore:
	en->rx_ignored++;
}

/* Send a debugnet message and wait, bounded, for its ACK. */
static int
send_message(struct earlynet *en, uint32_t type, const uint8_t *data, size_t len)
{
	uint8_t msg[DN_HDR + EARLYNET_MAX_PAYLOAD];
	unsigned attempt, polls;
	uint32_t seq = en->tx_seq;

	if (len > EARLYNET_MAX_PAYLOAD)
		return (EINVAL);
	put32(msg, type);
	put32(msg + 4, seq);
	memset(msg + 8, 0, 8);
	put32(msg + 16, (uint32_t)len);
	put32(msg + 20, 0);
	if (len != 0)
		memcpy(msg + DN_HDR, data, len);
	en->acked = 0;
	for (attempt = 0; attempt < EARLYNET_RETRIES; attempt++) {
		(void)send_udp(en, msg, DN_HDR + len);
		for (polls = 0; polls < EARLYNET_ACK_POLLS; polls++) {
			poll_once(en);
			if (en->acked == seq) {
				en->tx_seq++;
				return (0);
			}
			en->pf->delay_us(en->pf->ctx, EARLYNET_POLL_US);
		}
	}
	/* Like debugnet, the sequence is not advanced after a timeout. */
	en->send_timeouts++;
	en->degraded = true;
	return (ETIMEDOUT);
}

static int
resolve_peer(struct earlynet *en)
{
	unsigned attempt, polls;

	for (attempt = 0; attempt < EARLYNET_RETRIES && !en->have_peer_mac; attempt++) {
		(void)send_arp(en, 1, broadcast, en->peer_ip);
		for (polls = 0; polls < EARLYNET_ACK_POLLS && !en->have_peer_mac; polls++) {
			poll_once(en);
			en->pf->delay_us(en->pf->ctx, EARLYNET_POLL_US);
		}
	}
	return (en->have_peer_mac ? 0 : ETIMEDOUT);
}

int
earlynet_start(struct earlynet *en)
{
	int error;

	if (en->owner != EARLYNET_UNCLAIMED || en->be == NULL)
		return (EBUSY);
	en->owner = EARLYNET_OWNED;
	en->server_port = EARLYNET_SERVER_PORT;
	en->tx_seq = 1;
	error = en->be->attach(en);
	if (error == 0)
		error = resolve_peer(en);
	if (error == 0)
		error = send_message(en, DN_HERALD, (const uint8_t *)en->boot_id,
		    strlen(en->boot_id));
	if (error != 0) {
		/* Never leave a half-started device running. */
		(void)earlynet_quiesce(en);
		return (error);
	}
	en->admitted = true;
	return (0);
}

int
earlynet_flush(struct earlynet *en)
{
	size_t n;
	int error;

	if (en->owner != EARLYNET_OWNED || !en->admitted || en->busy || en->oc == 0)
		return (0);
	en->busy = true;
	n = en->oc < EARLYNET_MAX_PAYLOAD ? en->oc : EARLYNET_MAX_PAYLOAD;
	error = send_message(en, DN_DATA, en->out, n);
	/* Sent or not, the bytes leave the buffer; a timeout counts as loss. */
	memmove(en->out, en->out + n, en->oc - n);
	en->oc -= n;
	if (error != 0)
		en->lost_output += n;
	en->busy = false;
	return (error);
}

/*
 * Opportunistic flush for boot progress, called by the kernel glue after a
 * newline outside critical sections.  After a send timeout it waits for the
 * controller to be heard from again, so a missing controller costs one
 * bounded timeout, not one per line.
 */
int
earlynet_flush_line(struct earlynet *en)
{
	if (en->degraded)
		return (0);
	return (earlynet_flush(en));
}

void
earlynet_putc(struct earlynet *en, int c)
{
	if (en->owner != EARLYNET_OWNED || !en->admitted || en->busy)
		return;
	/* putc may run in a critical section: only buffer here. */
	if (en->oc == EARLYNET_OUTPUT) {
		memmove(en->out, en->out + 1, EARLYNET_OUTPUT - 1);
		en->oc--;
		en->lost_output++;
	}
	en->out[en->oc++] = (uint8_t)c;
}

int
earlynet_getc(struct earlynet *en)
{
	int c;

	if (en->owner != EARLYNET_OWNED || en->busy)
		return (-1);
	(void)earlynet_flush(en);
	if (en->ic == 0) {
		en->busy = true;
		poll_once(en);
		en->busy = false;
	}
	if (en->ic == 0)
		return (-1);
	c = en->in[en->ih];
	en->ih = (en->ih + 1) % EARLYNET_INPUT;
	en->ic--;
	return (c);
}

/* Tell the controller this transport ends; the next console reheralds. */
int
earlynet_finish(struct earlynet *en)
{
	int error;

	if (en->owner != EARLYNET_OWNED || !en->admitted)
		return (0);
	while (en->oc != 0)
		if (earlynet_flush(en) != 0)
			break;
	error = send_message(en, DN_FINISHED, NULL, 0);
	en->admitted = false;
	return (error);
}

int
earlynet_quiesce(struct earlynet *en)
{
	if (en->owner != EARLYNET_OWNED)
		return (en->owner == EARLYNET_RELEASED ? 0 : EINVAL);
	en->admitted = false;
	if (en->be->quiesce(en) != 0) {
		en->owner = EARLYNET_FAILED;
		return (EIO);
	}
	en->owner = EARLYNET_RELEASED;
	return (0);
}

bool
earlynet_driver_may_attach(const struct earlynet *en)
{
	return (en->owner == EARLYNET_UNCLAIMED || en->owner == EARLYNET_RELEASED);
}

const struct earlynet_backend *
earlynet_backend_for(uint16_t vendor, uint16_t device)
{
	if (vendor == 0x8086 && (device == 0x100e || device == 0x100f))
		return (&earlynet_em_backend);
	if (vendor == 0x1af4 && device == 0x1000)
		return (&earlynet_vtnet_backend);
	return (NULL);
}
