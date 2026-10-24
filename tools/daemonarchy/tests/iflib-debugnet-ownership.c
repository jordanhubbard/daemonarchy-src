/* SPDX-License-Identifier: BSD-2-Clause */
/* Exercise the verbatim production callback with distinct allocator objects. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error "Ownership checks require assertions"
#endif
#define MPASS(expression) assert(expression)
typedef void *uma_zone_t;
enum debugnet_ev { DEBUGNET_START, DEBUGNET_END };
struct iflib_fl {
	unsigned ifl_buf_size;
	uma_zone_t ifl_zone, ifl_debugnet_saved_zone;
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
typedef if_ctx_t if_t;
static int iflib_no_tx_batch;
static int original[2][3], reserved[3];
static if_ctx_t if_getsoftc(if_t ifp) { return (ifp); }
static uma_zone_t m_getzone(unsigned size)
{
	assert(size < 3);
	return (&reserved[size]);
}

/* PRODUCTION_FUNCTIONS */

int
main(int argc, char **argv)
{
	struct iflib_fl lists[2][3];
	struct iflib_rxq queues[2];
	struct iflib_ctx ctx;
	int i, j, cycle, prior, nrx, nfl;
	bool multiple, zones;

	assert(argc == 2);
	multiple = strcmp(argv[1], "all-lists") == 0;
	zones = strcmp(argv[1], "zones") == 0;
	assert(multiple || zones || strcmp(argv[1], "restore") == 0);
	nrx = multiple ? 2 : 1;
	nfl = multiple ? 3 : 1;
	for (prior = zones ? 1 : 0; prior <= (zones ? 1 : 2); prior++) {
		memset(&ctx, 0, sizeof(ctx));
		memset(lists, 0, sizeof(lists));
		ctx.ifc_rxqs = queues;
		ctx.ifc_softc_ctx.isc_nrxqsets = nrx;
		iflib_no_tx_batch = prior;
		for (i = 0; i < nrx; i++) {
			queues[i].ifr_nfl = nfl;
			queues[i].ifr_fl = lists[i];
			for (j = 0; j < nfl; j++) {
				lists[i][j].ifl_buf_size = j;
				lists[i][j].ifl_zone = &original[i][j];
			}
		}
		for (cycle = 0; cycle < 2; cycle++) {
			iflib_debugnet_event(&ctx, DEBUGNET_START);
			assert(iflib_no_tx_batch == 1);
			for (i = 0; i < nrx; i++)
				for (j = 0; j < nfl; j++)
					assert(lists[i][j].ifl_zone == &reserved[j]);
			iflib_debugnet_event(&ctx, DEBUGNET_END);
			assert(iflib_no_tx_batch == prior);
			for (i = 0; i < nrx; i++)
				for (j = 0; j < nfl; j++) {
					assert(lists[i][j].ifl_zone == &original[i][j]);
					assert(lists[i][j].ifl_debugnet_saved_zone == NULL);
				}
			assert(!ctx.ifc_debugnet_active);
		}
	}
	puts("all allocator pointers and prior batching values restored over repeated sessions");
	return (0);
}
