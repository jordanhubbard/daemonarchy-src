/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Receive-buffer allocation-class checks around the verbatim production
 * debugnet reserve, activation, iflib event and free-list cleanup functions.
 * Every ordinary allocation records its originating zone; freeing it through
 * any other ordinary zone fails the allocator-origin oracle.
 */
#include <sys/queue.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NDEBUG
#error "Allocation-class checks require assertions"
#endif

#define	MPASS(expression) assert(expression)
#define	KASSERT(expression, message) assert(expression)
#define	panic(...) do {							\
	fprintf(stderr, __VA_ARGS__);					\
	fputc('\n', stderr);						\
	abort();							\
} while (0)
#ifndef __unused
#define	__unused __attribute__((__unused__))
#endif
#ifndef nitems
#define	nitems(x) (sizeof((x)) / sizeof((x)[0]))
#endif
#define	bzero(p, n) memset((p), 0, (n))
#define	DEBUGNET 1
#define	INVARIANTS 1
#define	MEMORY_LOGGING 0
#define	M_NOWAIT 0x0001
#define	M_WAITOK 0x0002
#define	MT_DATA 1
#define	MSIZE 256
#define	MCLBYTES 2048
#define	MJUMPAGESIZE 4096
#define	MJUM9BYTES (9 * 1024)
#define	MJUM16BYTES (16 * 1024)
#define	UMA_ZONE_NOBUCKET 0x8000
#define	MBUF_MEM_NAME "mbuf"
#define	MBUF_CLUSTER_MEM_NAME "mbuf_cluster"
#define	MBUF_PACKET_MEM_NAME "mbuf_packet"
#define	MBUF_JUMBOP_MEM_NAME "mbuf_jumbo_page"
#define	MBUF_JUMBO9_MEM_NAME "mbuf_jumbo_9k"
#define	MBUF_JUMBO16_MEM_NAME "mbuf_jumbo_16k"
#define	IFT_ETHER 0x6
#define	BUS_DMASYNC_POSTREAD 2

typedef unsigned int u_int;
typedef char *caddr_t;
typedef uint16_t qidx_t;
typedef void *bus_dmamap_t;
typedef void *bus_dma_tag_t;

struct mbuf {
	STAILQ_ENTRY(mbuf) m_stailqpkt;
	struct {
		char *ext_buf;
		int ext_size;
	} m_ext;
};

struct mbufq {
	STAILQ_HEAD(, mbuf) mq_head;
	int mq_len;
	int mq_maxlen;
};

static struct mbuf *
mbufq_dequeue(struct mbufq *mq)
{
	struct mbuf *m;

	m = STAILQ_FIRST(&mq->mq_head);
	if (m != NULL) {
		STAILQ_REMOVE_HEAD(&mq->mq_head, m_stailqpkt);
		m->m_stailqpkt.stqe_next = NULL;
		mq->mq_len--;
	}
	return (m);
}

static int
mbufq_enqueue(struct mbufq *mq, struct mbuf *m)
{
	if (mq->mq_len >= mq->mq_maxlen)
		return (ENOBUFS);
	STAILQ_INSERT_TAIL(&mq->mq_head, m, m_stailqpkt);
	mq->mq_len++;
	return (0);
}

/* Fake UMA with an allocator-origin oracle. */
typedef int (*uma_ctor)(void *, int, void *, int);
typedef void (*uma_dtor)(void *, int, void *);
typedef int (*uma_init)(void *, int, int);
typedef void (*uma_fini)(void *, int);
typedef int (*uma_import)(void *, void **, int, int, int);
typedef void (*uma_release)(void *, void **, int);

struct uma_zone {
	const char *uz_name;
	int uz_size;
	bool uz_cache;
	uma_import uz_import;
	uma_release uz_release;
	void *uz_arg;
};
typedef struct uma_zone *uma_zone_t;

static struct uma_zone ordinary[] = {
	{ .uz_name = MBUF_MEM_NAME, .uz_size = MSIZE },
	{ .uz_name = MBUF_CLUSTER_MEM_NAME, .uz_size = MCLBYTES },
	{ .uz_name = MBUF_PACKET_MEM_NAME, .uz_size = MCLBYTES },
	{ .uz_name = MBUF_JUMBOP_MEM_NAME, .uz_size = MJUMPAGESIZE },
	{ .uz_name = MBUF_JUMBO9_MEM_NAME, .uz_size = MJUM9BYTES },
	{ .uz_name = MBUF_JUMBO16_MEM_NAME, .uz_size = MJUM16BYTES },
};
uma_zone_t zone_mbuf, zone_clust, zone_pack;
uma_zone_t zone_jumbop, zone_jumbo9, zone_jumbo16;

#define	MAXBUFS 4096
static struct {
	void *b_item;
	uma_zone_t b_origin;
} bufs[MAXBUFS];
static unsigned long ordinary_allocs;
static int live_bufs;

static bool
is_ordinary(uma_zone_t zone)
{
	return (zone >= &ordinary[0] && zone < &ordinary[nitems(ordinary)]);
}

static int
buf_index(void *item)
{
	int i;

	for (i = 0; i < MAXBUFS; i++)
		if (bufs[i].b_item == item)
			return (i);
	return (-1);
}

static uma_zone_t
uma_zcache_create(const char *name, int size, uma_ctor ctor __unused,
    uma_dtor dtor __unused, uma_init zinit __unused, uma_fini zfini __unused,
    uma_import zimport, uma_release zrelease, void *arg, int flags)
{
	uma_zone_t zone;

	assert(flags == UMA_ZONE_NOBUCKET);
	zone = calloc(1, sizeof(*zone));
	assert(zone != NULL);
	*zone = (struct uma_zone){ .uz_name = name, .uz_size = size,
	    .uz_cache = true, .uz_import = zimport, .uz_release = zrelease,
	    .uz_arg = arg };
	return (zone);
}

static void
uma_zdestroy(uma_zone_t zone)
{
	assert(zone->uz_cache);
	free(zone);
}

static void *
uma_zalloc(uma_zone_t zone, int flags)
{
	void *item;
	int i;

	if (zone->uz_cache)
		return (zone->uz_import(zone->uz_arg, &item, 1, -1, flags) == 1 ?
		    item : NULL);
	assert(is_ordinary(zone) && zone != &ordinary[2]);
	i = buf_index(NULL);
	assert(i >= 0);
	item = calloc(1, zone->uz_size);
	assert(item != NULL);
	bufs[i].b_item = item;
	bufs[i].b_origin = zone;
	ordinary_allocs++;
	live_bufs++;
	return (item);
}

static void
uma_zfree(uma_zone_t zone, void *item)
{
	int i;

	if (item == NULL)
		return;
	if (zone->uz_cache) {
		zone->uz_release(zone->uz_arg, &item, 1);
		return;
	}
	i = buf_index(item);
	assert(i >= 0);
	if (bufs[i].b_origin != zone) {
		fprintf(stderr, "allocator-origin oracle: %s buffer freed "
		    "through %s zone\n", bufs[i].b_origin->uz_name,
		    zone->uz_name);
		abort();
	}
	bufs[i].b_item = NULL;
	bufs[i].b_origin = NULL;
	live_bufs--;
	free(item);
}

/* Debug trash fill covers the whole item, exposing undersized buffers. */
static int
trash_init(void *mem, int size, int flags __unused)
{
	memset(mem, 0xde, size);
	return (0);
}

static int
mb_ctor_mbuf(void *mem __unused, int size __unused, void *arg __unused,
    int how __unused)
{
	return (0);
}

static void
mb_dtor_mbuf(void *mem __unused, int size __unused, void *arg __unused)
{
}

static int
mb_ctor_pack(void *mem __unused, int size __unused, void *arg __unused,
    int how __unused)
{
	return (0);
}

static void
mb_dtor_pack(void *mem __unused, int size __unused, void *arg __unused)
{
}

static int
mb_ctor_clust(void *mem, int size, void *arg, int how __unused)
{
	struct mbuf *m;

	m = arg;
	if (m != NULL) {
		m->m_ext.ext_buf = mem;
		m->m_ext.ext_size = size;
	}
	return (0);
}

static struct mbuf *
m_get(int how, short type __unused)
{
	return (uma_zalloc(zone_mbuf, how));
}

static struct mbuf *
m_free(struct mbuf *m)
{
	uma_zfree(zone_mbuf, m);
	return (NULL);
}

/* Fake network interface and iflib structures. */
typedef struct ifnet *if_t;
struct debugnet_methods {
	void (*dn_init)(if_t, int *, int *, int *);
};
struct ifnet {
	const struct debugnet_methods *if_debugnet_methods;
	int if_type;
	void *if_softc;
};
#define	DEBUGNET_SUPPORTED_NIC(ifp)					\
	((ifp)->if_debugnet_methods != NULL && (ifp)->if_type == IFT_ETHER)
enum debugnet_ev { DEBUGNET_START, DEBUGNET_END };
int bootverbose;
struct mtx { int mtx_unused; };
#define	MTX_SYSINIT(name, mtx, desc, opts) extern int name##_fixture_sysinit
#define	mtx_lock(m) ((void)(m))
#define	mtx_unlock(m) ((void)(m))

static const char *
if_name(if_t ifp __unused)
{
	return ("fixture0");
}

static void *
if_getsoftc(if_t ifp)
{
	return (ifp->if_softc);
}

typedef struct iflib_dma_info {
	caddr_t idi_vaddr;
	uint32_t idi_size;
} *iflib_dma_info_t;
typedef struct iflib_sw_rx_desc_array {
	bus_dmamap_t *ifsd_map;
	struct mbuf **ifsd_m;
	caddr_t *ifsd_cl;
} iflib_rxsd_array_t;
struct iflib_fl {
	qidx_t ifl_cidx, ifl_pidx, ifl_credits;
	uint8_t ifl_gen;
	qidx_t ifl_fragidx;
	qidx_t ifl_size;
	uint16_t ifl_buf_size;
	uma_zone_t ifl_zone;
	uma_zone_t ifl_debugnet_saved_zone;
	iflib_rxsd_array_t ifl_sds;
	bus_dma_tag_t ifl_buf_tag;
	iflib_dma_info_t ifl_ifdi;
};
typedef struct iflib_fl *iflib_fl_t;
struct iflib_rxq {
	int ifr_nfl;
	iflib_fl_t ifr_fl;
};
typedef struct iflib_rxq *iflib_rxq_t;
struct if_softc_ctx { int isc_nrxqsets; };
typedef struct if_softc_ctx *if_softc_ctx_t;
struct iflib_ctx {
	struct if_softc_ctx ifc_softc_ctx;
	iflib_rxq_t ifc_rxqs;
	int ifc_debugnet_saved_no_tx_batch;
	bool ifc_debugnet_active;
};
typedef struct iflib_ctx *if_ctx_t;
#define	CTX_LOCK(ctx) ((void)(ctx))
#define	CTX_UNLOCK(ctx) ((void)(ctx))
#define	NRXQSETS(ctx) ((ctx)->ifc_softc_ctx.isc_nrxqsets)
static int iflib_no_tx_batch;

static void
bus_dmamap_sync(bus_dma_tag_t tag __unused, bus_dmamap_t map __unused,
    int op __unused)
{
}

static void
bus_dmamap_unload(bus_dma_tag_t tag __unused, bus_dmamap_t map __unused)
{
}

static int
m_init(struct mbuf *m __unused, int how __unused, short type __unused,
    int flags __unused)
{
	abort();
}

static void
m_free_raw(struct mbuf *m __unused)
{
	abort();
}

/* PRODUCTION_FUNCTIONS */

/* Controlled harness: one receive queue with one free list per interface. */
#define	RING 8
struct nic {
	struct ifnet ifp;
	struct iflib_ctx ctx;
	struct iflib_rxq rxq;
	struct iflib_fl fl;
	struct iflib_dma_info idi;
	char descriptors[64];
	bus_dmamap_t maps[RING];
	struct mbuf *ms[RING];
	caddr_t cls[RING];
	void *retained;
};
static const struct debugnet_methods iflib_methods = {
	.dn_init = iflib_debugnet_init,
};

static void
nic_attach(struct nic *nic, int bufsize)
{
	memset(nic, 0, sizeof(*nic));
	nic->ifp = (struct ifnet){ .if_debugnet_methods = &iflib_methods,
	    .if_type = IFT_ETHER, .if_softc = &nic->ctx };
	nic->ctx.ifc_softc_ctx.isc_nrxqsets = 1;
	nic->ctx.ifc_rxqs = &nic->rxq;
	nic->rxq = (struct iflib_rxq){ .ifr_nfl = 1, .ifr_fl = &nic->fl };
	nic->idi = (struct iflib_dma_info){ .idi_vaddr = nic->descriptors,
	    .idi_size = sizeof(nic->descriptors) };
	nic->fl.ifl_size = RING;
	nic->fl.ifl_buf_size = bufsize;
	nic->fl.ifl_zone = m_getzone(bufsize);	/* iflib_fl_setup() */
	nic->fl.ifl_sds = (iflib_rxsd_array_t){ .ifsd_map = nic->maps,
	    .ifsd_m = nic->ms, .ifsd_cl = nic->cls };
	nic->fl.ifl_ifdi = &nic->idi;
}

/* Controlled refill: allocate empty descriptors from the current list zone. */
static void
nic_refill(struct nic *nic)
{
	int i;

	for (i = 0; i < RING; i++)
		if (nic->cls[i] == NULL) {
			nic->cls[i] = uma_zalloc(nic->fl.ifl_zone, M_NOWAIT);
			assert(nic->cls[i] != NULL);
		}
}

/*
 * Receive half the ring: one buffer stays with the stack beyond the session,
 * the others are freed through the global zone of the list's cluster type,
 * as mb_free_ext() does.
 */
static void
nic_receive(struct nic *nic)
{
	int i;

	for (i = 0; i < RING / 2; i++) {
		if (nic->retained == NULL)
			nic->retained = nic->cls[i];
		else
			uma_zfree(m_getzone(nic->fl.ifl_buf_size), nic->cls[i]);
		nic->cls[i] = NULL;
	}
}

static void
session(struct nic **nics, int n, bool pack)
{
	struct mbuf *pkt;
	unsigned long before;
	int i;

	before = ordinary_allocs;
	assert(debugnet_mbuf_start() == 0);
	for (i = 0; i < n; i++)
		iflib_debugnet_event(&nics[i]->ifp, DEBUGNET_START);
	for (i = 0; i < n; i++) {
		nic_receive(nics[i]);
		nic_refill(nics[i]);
	}
	/* A packet-zone buffer carries a cluster of the ordinary class. */
	if (pack) {
		pkt = uma_zalloc(zone_pack, M_NOWAIT);
		assert(pkt != NULL);
		assert(pkt->m_ext.ext_size == MCLBYTES);
		i = buf_index(pkt->m_ext.ext_buf);
		assert(i >= 0 && bufs[i].b_origin == &ordinary[1]);
		uma_zfree(zone_pack, pkt);
	}
	/* No physical allocation while the reserve is active. */
	assert(ordinary_allocs == before);
	for (i = 0; i < n; i++)
		iflib_debugnet_event(&nics[i]->ifp, DEBUGNET_END);
	debugnet_mbuf_finish();
	assert(zone_mbuf == &ordinary[0] && zone_clust == &ordinary[1] &&
	    zone_pack == &ordinary[2] && zone_jumbop == &ordinary[3] &&
	    zone_jumbo9 == &ordinary[4] && zone_jumbo16 == &ordinary[5]);
	for (i = 0; i < n; i++)
		assert(nics[i]->fl.ifl_zone == m_getzone(nics[i]->fl.ifl_buf_size));
	/* Buffers kept by the stack are freed through the restored zone. */
	for (i = 0; i < n; i++) {
		uma_zfree(m_getzone(nics[i]->fl.ifl_buf_size), nics[i]->retained);
		nics[i]->retained = NULL;
	}
}

static void
cleanup(struct nic **nics, int n)
{
	int i;

	for (i = 0; i < n; i++)
		iflib_fl_bufs_free(&nics[i]->fl);
	debugnet_mbuf_drain();
	/* Ownership conservation: every ordinary buffer went home once. */
	assert(live_bufs == 0);
}

int
main(int argc, char **argv)
{
	struct nic small, large;
	struct nic *nics[2] = { &small, &large };
	int cycle;

	zone_mbuf = &ordinary[0]; zone_clust = &ordinary[1];
	zone_pack = &ordinary[2]; zone_jumbop = &ordinary[3];
	zone_jumbo9 = &ordinary[4]; zone_jumbo16 = &ordinary[5];
	assert(argc == 2);
	if (strcmp(argv[1], "matching") == 0) {
		nic_attach(&small, MCLBYTES);
		debugnet_any_ifnet_update(&small.ifp);
		nic_refill(&small);
		session(nics, 1, true);
		cleanup(nics, 1);
		puts("matching: 2048-byte reserve and 2048-byte ring cleanup passed");
		return (0);
	}
	if (strcmp(argv[1], "jumbo-reserve") == 0) {
		/* Only the 2048-byte list runs while a 9216-byte NIC exists. */
		nic_attach(&small, MCLBYTES);
		nic_attach(&large, MJUM9BYTES);
		debugnet_any_ifnet_update(&large.ifp);
		debugnet_any_ifnet_update(&small.ifp);
		nic_refill(&small);
		session(nics, 1, false);
		cleanup(nics, 1);
		puts("jumbo-reserve: 2048-byte ring cleanup returned 2048-byte buffers");
		return (0);
	}
	if (strcmp(argv[1], "mixed") == 0) {
		nic_attach(&small, MCLBYTES);
		nic_attach(&large, MJUM9BYTES);
		debugnet_any_ifnet_update(&large.ifp);
		debugnet_any_ifnet_update(&small.ifp);
		nic_refill(&small);
		nic_refill(&large);
		session(nics, 2, true);
		cleanup(nics, 2);
		puts("mixed: 9216-byte and 2048-byte reserves kept their classes");
		return (0);
	}
	if (strcmp(argv[1], "repeated") == 0) {
		nic_attach(&small, MCLBYTES);
		nic_attach(&large, MJUM9BYTES);
		assert(debugnet_mbuf_start() == ENXIO);
		debugnet_any_ifnet_update(&small.ifp);
		debugnet_any_ifnet_update(&large.ifp);
		nic_refill(&small);
		nic_refill(&large);
		for (cycle = 0; cycle < 3; cycle++) {
			session(nics, 2, true);
			/* Link renegotiation between sessions. */
			debugnet_any_ifnet_update(&nics[cycle % 2]->ifp);
			nic_refill(&small);
			nic_refill(&large);
		}
		cleanup(nics, 2);
		puts("repeated: refusal, three sessions and cleanup kept classes");
		return (0);
	}
	return (2);
}
