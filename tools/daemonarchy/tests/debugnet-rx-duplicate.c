/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Duplicate-input suppression around the verbatim production debugnet
 * receive path: a retransmission of the last accepted DATA message is
 * acknowledged again but never delivered to the consumer twice.
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error "Duplicate suppression checks require assertions"
#endif

#define	DEBUGNET_HERALD		1
#define	DEBUGNET_FINISHED	2
#define	DEBUGNET_DATA		3
#define	DNETDEBUG(...)		do { } while (0)
#define	mtod(m, t)		((t)((m)->m_data))

struct pkthdr { int len; };
struct mbuf {
	struct mbuf *m_next;
	char *m_data;
	int m_len;
	struct pkthdr m_pkthdr;
};

struct debugnet_msg_hdr {
	uint32_t mh_type;
	uint32_t mh_seqno;
	uint64_t mh_offset;
	uint32_t mh_len;
	uint32_t mh_aux2;
} __attribute__((__packed__));

enum dnet_pcb_st { DN_STATE_INIT = 1, DN_STATE_HAVE_GW_MAC,
    DN_STATE_GOT_HERALD_PORT, DN_STATE_REMOTE_CLOSED };

/* Fields used by the selected functions, as in sys/net/debugnet_int.h. */
struct debugnet_pcb {
	int (*dp_rx_handler)(struct mbuf *);
	void (*dp_finish_handler)(void);
	enum dnet_pcb_st dp_state;
	bool dp_rx_accepted;
	uint32_t dp_rx_last_seqno;
	uint32_t dp_rx_last_len;
	uint32_t dp_rx_last_digest;
};

static uint32_t acks[64];
static int nacks;
static char delivered[1024];
static size_t ndelivered;
static int deliveries, finishes, refuse_next;

static struct mbuf *
m_pullup(struct mbuf *m, int len)
{
	assert(m->m_len >= len);
	return (m);
}

static void
m_adj(struct mbuf *m, int len)
{
	assert(len <= m->m_len);
	m->m_data += len;
	m->m_len -= len;
	m->m_pkthdr.len -= len;
}

static int
debugnet_ack_output(struct debugnet_pcb *pcb __attribute__((unused)), uint32_t seqno)
{
	assert(nacks < 64);
	acks[nacks++] = ntohl(seqno);
	return (0);
}

/* NetGDB-like consumer: appends every accepted payload. */
static int
consumer(struct mbuf *m)
{
	int remaining = m->m_pkthdr.len;

	if (refuse_next) {
		refuse_next = 0;
		return (ENOBUFS);
	}
	for (; m != NULL && remaining > 0; m = m->m_next) {
		int n = m->m_len < remaining ? m->m_len : remaining;
		assert(ndelivered + n <= sizeof(delivered));
		memcpy(delivered + ndelivered, m->m_data, n);
		ndelivered += n;
		remaining -= n;
	}
	deliveries++;
	return (0);
}

static void
finished(void)
{
	finishes++;
}

/* PRODUCTION_FUNCTIONS */

static char storage[8][256];
static struct mbuf chain[8];

/* Build a message, optionally split after `split` payload bytes. */
static struct mbuf *
message(int slot, uint32_t type, uint32_t seq, const char *payload, int split)
{
	struct debugnet_msg_hdr hdr = { htonl(type), htonl(seq), 0,
	    htonl((uint32_t)strlen(payload)), 0 };
	int len = (int)strlen(payload);
	char *buf = storage[slot];

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy(buf + sizeof(hdr), payload, len);
	chain[slot] = (struct mbuf){ NULL, buf, (int)sizeof(hdr) + len,
	    { (int)sizeof(hdr) + len } };
	if (split > 0 && split < len) {
		chain[slot].m_len = (int)sizeof(hdr) + split;
		chain[slot + 1] = (struct mbuf){ NULL, buf + sizeof(hdr) + split,
		    len - split, { 0 } };
		chain[slot].m_next = &chain[slot + 1];
	}
	return (&chain[slot]);
}

static void
receive(struct debugnet_pcb *pcb, struct mbuf *m)
{
	debugnet_handle_rx_msg(pcb, &m);
}

static struct debugnet_pcb
pcb_new(void)
{
	return ((struct debugnet_pcb){ .dp_rx_handler = consumer,
	    .dp_finish_handler = finished, .dp_state = DN_STATE_GOT_HERALD_PORT });
}

static void
lost_ack_retransmission(void)
{
	struct debugnet_pcb pcb = pcb_new();

	receive(&pcb, message(0, DEBUGNET_DATA, 1, "$g#67", 0));
	receive(&pcb, message(0, DEBUGNET_DATA, 1, "$g#67", 0));
	receive(&pcb, message(0, DEBUGNET_DATA, 1, "$g#67", 0));
	assert(deliveries == 1);
	assert(ndelivered == 5 && memcmp(delivered, "$g#67", 5) == 0);
	assert(nacks == 3 && acks[0] == 1 && acks[1] == 1 && acks[2] == 1);
	receive(&pcb, message(0, DEBUGNET_DATA, 2, "$c#63", 0));
	assert(deliveries == 2 && ndelivered == 10);
}

static void
restarted_peer_reuses_sequence(void)
{
	struct debugnet_pcb pcb = pcb_new();

	receive(&pcb, message(0, DEBUGNET_DATA, 1, "$?#3f", 0));
	receive(&pcb, message(0, DEBUGNET_DATA, 1, "$g#67", 0));
	assert(deliveries == 2);
	assert(memcmp(delivered, "$?#3f$g#67", 10) == 0);
}

static void
refused_message_is_not_remembered(void)
{
	struct debugnet_pcb pcb = pcb_new();

	refuse_next = 1;
	receive(&pcb, message(0, DEBUGNET_DATA, 7, "abc", 0));
	assert(deliveries == 0 && nacks == 0);
	receive(&pcb, message(0, DEBUGNET_DATA, 7, "abc", 0));
	assert(deliveries == 1 && nacks == 1 && acks[0] == 7);
}

static void
chained_payload_matches(void)
{
	struct debugnet_pcb pcb = pcb_new();

	receive(&pcb, message(0, DEBUGNET_DATA, 3, "split-payload", 5));
	receive(&pcb, message(2, DEBUGNET_DATA, 3, "split-payload", 0));
	assert(deliveries == 1 && nacks == 2);
	receive(&pcb, message(4, DEBUGNET_DATA, 3, "split-paylaod", 9));
	assert(deliveries == 2);
}

static void
new_session_forgets(void)
{
	struct debugnet_pcb pcb = pcb_new();

	receive(&pcb, message(0, DEBUGNET_DATA, 1, "x", 0));
	pcb = pcb_new();	/* debugnet_connect() builds a fresh pcb */
	receive(&pcb, message(0, DEBUGNET_DATA, 1, "x", 0));
	assert(deliveries == 2);
}

static void
finish_and_short(void)
{
	struct debugnet_pcb pcb = pcb_new();
	struct mbuf tiny = { NULL, storage[6], 4, { 4 } };

	receive(&pcb, &tiny);
	receive(&pcb, message(0, DEBUGNET_FINISHED, 9, "", 0));
	assert(finishes == 1 && pcb.dp_state == DN_STATE_REMOTE_CLOSED);
	assert(deliveries == 0 && nacks == 0);
}

int
main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } cases[] = {
		{ "lost-ack", lost_ack_retransmission },
		{ "restart-reuse", restarted_peer_reuses_sequence },
		{ "refused", refused_message_is_not_remembered },
		{ "chained", chained_payload_matches },
		{ "new-session", new_session_forgets },
		{ "finish-short", finish_and_short },
	};
	size_t i;

	if (argc != 2)
		return (2);
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (strcmp(argv[1], cases[i].name) == 0) {
			cases[i].run();
			printf("%s: ok\n", argv[1]);
			return (0);
		}
	}
	return (2);
}
