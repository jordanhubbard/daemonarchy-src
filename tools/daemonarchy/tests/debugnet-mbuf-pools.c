/* SPDX-License-Identifier: BSD-2-Clause */
/* Allocator ownership checks around verbatim production activation functions. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef NDEBUG
#error "Allocator checks require assertions"
#endif
#define MPASS(expression) assert(expression)
#define nitems(x) (sizeof((x)) / sizeof((x)[0]))
typedef unsigned int u_int;
typedef void *uma_zone_t;
/* original: mbuf, cluster, packet, page, 9k, 16k; reserved: mbuf, packet, clusters */
static int original[6], reserved[6];
static uma_zone_t zone_mbuf, zone_clust, zone_pack;
static uma_zone_t zone_jumbop, zone_jumbo9, zone_jumbo16;
struct debugnet_pool {
	uma_zone_t dp_zone;
	uma_zone_t *dp_global;
};
static struct debugnet_pool dn_mbufs = { .dp_global = &zone_mbuf };
static struct debugnet_pool dn_clusts[] = {
	{ .dp_global = &zone_clust },
	{ .dp_global = &zone_jumbop },
	{ .dp_global = &zone_jumbo9 },
	{ .dp_global = &zone_jumbo16 },
};
static uma_zone_t dn_zone_pack;
static struct debugnet_saved_zones {
	bool dsz_debugnet_zones_enabled;
	uma_zone_t dsz_mbuf, dsz_clust, dsz_pack;
	uma_zone_t dsz_jumbop, dsz_jumbo9, dsz_jumbo16;
} dn_saved_zones;

/* PRODUCTION_FUNCTIONS */

static int
activate(void)
{
#if START_RETURNS_ERROR
	return (debugnet_mbuf_start());
#else
	debugnet_mbuf_start();
	return (0);
#endif
}

/* Mask bits: mbuf reserve, packet reserve, then one bit per cluster size. */
#define	ALL_POOLS 0x3f

static void
prepare(unsigned mask)
{
	unsigned i;

	dn_mbufs.dp_zone = (mask & 1) ? &reserved[0] : NULL;
	dn_zone_pack = (mask & 2) ? &reserved[1] : NULL;
	for (i = 0; i < nitems(dn_clusts); i++)
		dn_clusts[i].dp_zone = (mask & (4 << i)) ? &reserved[2 + i] :
		    NULL;
}

static void
reset(unsigned mask)
{
	zone_mbuf = &original[0]; zone_clust = &original[1];
	zone_pack = &original[2]; zone_jumbop = &original[3];
	zone_jumbo9 = &original[4]; zone_jumbo16 = &original[5];
	prepare(mask);
	memset(&dn_saved_zones, 0, sizeof(dn_saved_zones));
}

static void
original_zones(void)
{
	assert(zone_mbuf == &original[0]);
	assert(zone_clust == &original[1]);
	assert(zone_pack == &original[2]);
	assert(zone_jumbop == &original[3]);
	assert(zone_jumbo9 == &original[4]);
	assert(zone_jumbo16 == &original[5]);
	assert(!dn_saved_zones.dsz_debugnet_zones_enabled);
}

static void
ready_cycle(void)
{
	assert(activate() == 0);
	assert(dn_saved_zones.dsz_debugnet_zones_enabled);
	/* Every cluster size is served by the reserve of its own class. */
	assert(zone_mbuf == &reserved[0]);
	assert(zone_pack == &reserved[1]);
	assert(zone_clust == &reserved[2]);
	assert(zone_jumbop == &reserved[3]);
	assert(zone_jumbo9 == &reserved[4]);
	assert(zone_jumbo16 == &reserved[5]);
	debugnet_mbuf_finish();
	original_zones();
}

int
main(int argc, char **argv)
{
	unsigned mask;
	int error;

	if (argc == 2 && strcmp(argv[1], "ready") == 0) {
		reset(ALL_POOLS); ready_cycle(); ready_cycle();
		puts("ready: two complete activation/restoration cycles passed");
		return (0);
	}
	for (mask = 0; mask < ALL_POOLS; mask++) {
		reset(mask);
		error = activate();
		original_zones();
		assert(error == ENXIO);
		/* A refusal must leave activation available after preparation. */
		prepare(ALL_POOLS);
		ready_cycle();
	}
	puts("unready: 63 absent/partial pool and retry cases passed");
	return (0);
}
