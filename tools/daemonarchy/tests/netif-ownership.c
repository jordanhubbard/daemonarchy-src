/* SPDX-License-Identifier: BSD-2-Clause */
/* Exercise the production socket/interface owner, without firmware callbacks. */
#define panic fixture_panic
#define malloc fixture_malloc
#define calloc fixture_calloc
#define free fixture_free
#include <stand/libsa/netif.c>
#undef panic
#undef malloc
#undef calloc
#undef free
#include <assert.h>

extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
static unsigned allocation_calls, fail_allocation, live_allocations;

void *
fixture_malloc(size_t bytes)
{
	void *p;

	if (++allocation_calls == fail_allocation)
		return (NULL);
	p = malloc(bytes);
	if (p != NULL) live_allocations++;
	return (p);
}

void *
fixture_calloc(size_t count, size_t bytes)
{
	void *p;

	if (++allocation_calls == fail_allocation)
		return (NULL);
	p = calloc(count, bytes);
	if (p != NULL) live_allocations++;
	return (p);
}

void
fixture_free(void *p)
{
	if (p != NULL) { assert(live_allocations != 0); live_allocations--; }
	free(p);
}

static unsigned probes[2], starts[2], stops[2];
static unsigned configures[2];
static int refused_unit = -1;
static struct netif_stats statistics[2];
static struct netif_dif interfaces[2] = {
	{ .dif_unit = 0, .dif_nsel = 1, .dif_stats = &statistics[0] },
	{ .dif_unit = 1, .dif_nsel = 1, .dif_stats = &statistics[1] }
};
struct in_addr myip;
int errno;

void
fixture_panic(const char *message, ...)
{
	__builtin_trap();
}

static int
match(struct netif *nif, void *hint)
{
	return (nif->nif_unit == *(int *)hint);
}

static int
probe(struct netif *nif, void *hint)
{
	probes[nif->nif_unit]++;
	return (nif->nif_unit == refused_unit);
}

static void
start(struct iodesc *desc, void *hint)
{
	int unit = desc->io_netif->nif_unit;

	starts[unit]++;
	desc->myea[5] = 0x10 + unit;
	desc->io_netif->nif_devdata = &statistics[unit];
}

static void
stop(struct netif *nif)
{
	assert(nif->nif_devdata == &statistics[nif->nif_unit]);
	stops[nif->nif_unit]++;
	nif->nif_devdata = NULL;
}

static struct netif_driver driver = {
	.netif_bname = "fixture", .netif_match = match,
	.netif_probe = probe, .netif_init = start, .netif_end = stop,
	.netif_ifs = interfaces, .netif_nifs = 2
};
struct netif_driver *netif_drivers[] = { &driver, NULL };
int n_netif_drivers = 1;

static void
configure(struct iodesc *desc)
{
	int unit = desc->io_netif->nif_unit;

	configures[unit]++;
	desc->myip.s_addr = htonl(16 + unit);
}

int
main(void)
{
	struct netif *first;
	int unit0 = 0, unit1 = 1, a, b, shared, retry, failure;

	a = netif_open(&unit0);
	assert(a >= 0);
	first = socktodesc(a)->io_netif;
	b = netif_open(&unit1);
	assert(b >= 0 && b != a);
	assert(first->nif_unit == 0);
	assert(socktodesc(a)->io_netif != socktodesc(b)->io_netif);
	assert(socktodesc(a)->myea[5] == 0x10);
	assert(socktodesc(b)->myea[5] == 0x11);
	driver.netif_configure = configure;
	shared = netif_open(&unit0);
	assert(shared >= 0 && shared != a && shared != b);
	assert(socktodesc(shared)->io_netif == first);
	assert(probes[0] == 1 && starts[0] == 1);
	assert(configures[0] == 0 && configures[1] == 0);
	netif_configure(shared);
	assert(configures[0] == 1 && configures[1] == 0);
	assert(socktodesc(shared)->myip.s_addr == htonl(16));
	assert(socktodesc(a)->myip.s_addr == 0);
	assert(netif_close(a) == 0 && stops[0] == 0);
	assert(socktodesc(shared)->io_netif->nif_unit == 0);
	assert(netif_close(b) == 0 && stops[1] == 1 && stops[0] == 0);
	assert(netif_close(shared) == 0 && stops[0] == 1);
	assert(TAILQ_EMPTY(&sockets));
	assert(live_allocations == 0);
	driver.netif_configure = NULL;
	refused_unit = 1;
	assert(netif_open(&unit1) == -1);
	refused_unit = -1;
	retry = netif_open(&unit1);
	assert(retry >= 0 && socktodesc(retry)->io_netif->nif_unit == 1);
	assert(starts[1] == 2);
	assert(netif_close(retry) == 0 && stops[1] == 2);
	assert(TAILQ_EMPTY(&sockets));
	assert(live_allocations == 0);
	/* Both the socket and physical-owner allocations fail without leaks. */
	for (failure = 1; failure <= 2; failure++) {
		allocation_calls = 0; fail_allocation = failure;
		assert(netif_open(&unit0) == -1);
		assert(TAILQ_EMPTY(&sockets) && live_allocations == 0);
		fail_allocation = 0;
		retry = netif_open(&unit0);
		assert(retry >= 0);
		assert(netif_close(retry) == 0);
		assert(TAILQ_EMPTY(&sockets) && live_allocations == 0);
	}
	printf("netif distinct/shared lifetime and failed-probe retry checks passed\n");
	return (0);
}
