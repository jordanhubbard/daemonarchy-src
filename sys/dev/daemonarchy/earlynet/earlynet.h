/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */
/*
 * Kernel-owned, bounded, polled network console used before the ordinary
 * NIC driver attaches.  See tools/daemonarchy/earlynet-ownership.md for the
 * ownership and handoff contract.  The core is free of kernel dependencies
 * so the same code runs against simulated devices in host fixtures; all
 * device and memory access goes through struct earlynet_platform.
 *
 * Wire format: FreeBSD debugnet framing (UDP client port 26004 to server
 * port 26003, HERALD carrying the loader boot ID, DATA, FINISHED, 4-byte
 * ACKs), with the same four-byte input sequence inside controller DATA as
 * the diagnostic mountroot console, so one debugnet-console.py session
 * follows the early transport and then the post-attach console.
 */
#ifndef _EARLYNET_H_
#define	_EARLYNET_H_

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#define	EARLYNET_SERVER_PORT	26003
#define	EARLYNET_CLIENT_PORT	26004
#define	EARLYNET_INPUT		512
#define	EARLYNET_OUTPUT		1024
#define	EARLYNET_MAX_PAYLOAD	512
#define	EARLYNET_FRAME		1514
#define	EARLYNET_RX_SLOTS	8
#define	EARLYNET_BUF_SIZE	2048
/* Bounds: every wait below is a fixed number of polls of this length. */
#define	EARLYNET_POLL_US	100
#define	EARLYNET_ACK_POLLS	2000	/* 200 ms per attempt */
#define	EARLYNET_RETRIES	10
#define	EARLYNET_TX_POLLS	1000	/* 100 ms for one frame to leave */
#define	EARLYNET_RESET_POLLS	1000
#define	EARLYNET_RX_BUDGET	EARLYNET_RX_SLOTS

struct earlynet_dma {
	void		*va;
	uint64_t	 pa;
	size_t		 size;
};

struct earlynet_platform {
	void	 *ctx;
	uint32_t (*rd32)(void *, uint32_t);
	void	 (*wr32)(void *, uint32_t, uint32_t);
	uint16_t (*rd16)(void *, uint32_t);
	void	 (*wr16)(void *, uint32_t, uint16_t);
	uint8_t	 (*rd8)(void *, uint32_t);
	void	 (*wr8)(void *, uint32_t, uint8_t);
	int	 (*dma_alloc)(void *, size_t, size_t, struct earlynet_dma *);
	void	 (*dma_free)(void *, struct earlynet_dma *);
	void	 (*delay_us)(void *, unsigned);
	/* Order descriptor memory writes before a doorbell and after DMA. */
	void	 (*sync)(void *);
};

/*
 * Ownership of the selected PCI function.  Only EARLYNET_RELEASED allows the
 * native driver to attach; a failed quiesce leaves EARLYNET_FAILED, and the
 * device then stays claimed so no driver can race outstanding DMA.
 */
enum earlynet_owner {
	EARLYNET_UNCLAIMED = 0,
	EARLYNET_OWNED,
	EARLYNET_QUIESCED,
	EARLYNET_RELEASED,
	EARLYNET_FAILED
};

struct earlynet;

struct earlynet_backend {
	const char	*name;
	/* Reset, program rings, enable RX/TX with interrupts masked. */
	int	(*attach)(struct earlynet *);
	/* Queue one frame and wait, bounded, until the device consumed it. */
	int	(*send)(struct earlynet *, const uint8_t *, size_t);
	/* Hand at most `budget` received frames to earlynet_input(). */
	unsigned (*poll)(struct earlynet *, unsigned budget);
	/* Stop DMA and reset; on success no further DMA can occur. */
	int	(*quiesce)(struct earlynet *);
};

struct earlynet_em {
	struct earlynet_dma	rx_ring, tx_ring, bufs;
	unsigned		rx_next, tx_tail;
};

struct earlynet_vtnet {
	struct earlynet_dma	vq[2], bufs;
	uint16_t		num[2];
	uint16_t		avail_idx[2], used_seen[2];
};

struct earlynet {
	const struct earlynet_platform	*pf;
	const struct earlynet_backend	*be;
	enum earlynet_owner		 owner;
	uint8_t		mac[6], peer_mac[6];	/* mac: preset = expected */
	bool		have_peer_mac, admitted, busy;
	bool		degraded;	/* last send timed out */
	uint32_t	local_ip, peer_ip;	/* network order */
	uint16_t	server_port;		/* controller UDP port, host order */
	char		boot_id[64];
	uint32_t	tx_seq;			/* next outbound debugnet sequence */
	uint32_t	acked;			/* last ACK received */
	uint32_t	input_last;		/* last accepted inner input sequence */
	uint8_t		in[EARLYNET_INPUT];
	unsigned	ih, ic;
	uint8_t		out[EARLYNET_OUTPUT];
	size_t		oc;
	uint8_t		frame[EARLYNET_FRAME];
	uint64_t	rx_frames, rx_ignored, tx_frames, tx_failed, lost_output;
	uint64_t	send_timeouts, input_refused;
	union {
		struct earlynet_em	em;
		struct earlynet_vtnet	vt;
	} u;
};

extern const struct earlynet_backend earlynet_em_backend;
extern const struct earlynet_backend earlynet_vtnet_backend;

const struct earlynet_backend *earlynet_backend_for(uint16_t vendor,
	    uint16_t device);
int	earlynet_start(struct earlynet *);
void	earlynet_input(struct earlynet *, const uint8_t *, size_t);
int	earlynet_flush(struct earlynet *);
int	earlynet_flush_line(struct earlynet *);
int	earlynet_getc(struct earlynet *);
void	earlynet_putc(struct earlynet *, int);
int	earlynet_drain(struct earlynet *);
int	earlynet_finish(struct earlynet *);
int	earlynet_quiesce(struct earlynet *);
bool	earlynet_driver_may_attach(const struct earlynet *);

#endif /* !_EARLYNET_H_ */
