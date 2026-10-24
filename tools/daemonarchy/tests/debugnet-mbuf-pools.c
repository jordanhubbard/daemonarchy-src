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
typedef void *uma_zone_t;
static int original[6], reserved[3];
static uma_zone_t zone_mbuf, zone_clust, zone_pack;
static uma_zone_t zone_jumbop, zone_jumbo9, zone_jumbo16;
static uma_zone_t dn_zone_mbuf, dn_zone_clust, dn_zone_pack;
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

static void
reset(unsigned mask)
{
	zone_mbuf = &original[0]; zone_clust = &original[1];
	zone_pack = &original[2]; zone_jumbop = &original[3];
	zone_jumbo9 = &original[4]; zone_jumbo16 = &original[5];
	dn_zone_mbuf = (mask & 1) ? &reserved[0] : NULL;
	dn_zone_clust = (mask & 2) ? &reserved[1] : NULL;
	dn_zone_pack = (mask & 4) ? &reserved[2] : NULL;
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
	assert(zone_mbuf == &reserved[0]);
	assert(zone_clust == &reserved[1]);
	assert(zone_pack == &reserved[2]);
	assert(zone_jumbop == &reserved[1]);
	assert(zone_jumbo9 == &reserved[1]);
	assert(zone_jumbo16 == &reserved[1]);
	debugnet_mbuf_finish();
	original_zones();
}

int
main(int argc, char **argv)
{
	unsigned mask;
	int error;

	if (argc == 2 && strcmp(argv[1], "ready") == 0) {
		reset(7); ready_cycle(); ready_cycle();
		puts("ready: two complete activation/restoration cycles passed");
		return (0);
	}
	for (mask = 0; mask < 7; mask++) {
		reset(mask);
		error = activate();
		original_zones();
		assert(error == ENXIO);
		/* A refusal must leave activation available after preparation. */
		dn_zone_mbuf = &reserved[0];
		dn_zone_clust = &reserved[1];
		dn_zone_pack = &reserved[2];
		ready_cycle();
	}
	puts("unready: seven absent/partial pool and retry cases passed");
	return (0);
}
