/* SPDX-License-Identifier: BSD-2-Clause */
/* Production path parser: no firmware, disk or persistence acceptance. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "boottrace_media.h"

static void
put64(unsigned char *p, uint64_t value)
{
	unsigned i;
	for (i = 0; i < 8; i++) p[i] = value >> (8 * i);
}

static size_t
path(unsigned char *p, unsigned disk, unsigned partition, uint64_t start,
    uint64_t sectors, unsigned guid)
{
	memset(p, 0, 64);
	/* A PCI node gives the physical parent an identity. */
	p[0] = 1; p[1] = 1; p[2] = 6; p[4] = disk;
	p[6] = 4; p[7] = 1; p[8] = 42; p[10] = partition;
	put64(p + 14, start); put64(p + 22, sectors);
	p[30] = guid; p[46] = 2; p[47] = 2;
	p[48] = 0x7f; p[49] = 0xff; p[50] = 4;
	return (52);
}

int
main(void)
{
	unsigned char a[64], b[64], copy[64], oversized[4097];
	struct bt_media_path boot, candidate;
	size_t n, i;
	unsigned checks = 0;

	n = path(a, 1, 1, 80, 4096, 1);
	assert(bt_media_parse(a, n, &boot) == 0);
	path(b, 1, 2, 8192, 524288, 2);
	assert(bt_media_parse(b, n, &candidate) == 0);
	assert(bt_media_sibling(&boot, &candidate)); checks++;
	assert(!bt_media_sibling(&boot, &boot)); checks++;
	path(b, 2, 2, 8192, 524288, 2);
	assert(bt_media_parse(b, n, &candidate) == 0);
	assert(!bt_media_sibling(&boot, &candidate)); checks++;
	path(b, 1, 2, 8192, 524288, 1);
	assert(bt_media_parse(b, n, &candidate) == 0);
	assert(!bt_media_sibling(&boot, &candidate)); checks++;
	path(b, 1, 2, 100, 524288, 2);
	assert(bt_media_parse(b, n, &candidate) == 0);
	assert(!bt_media_sibling(&boot, &candidate)); checks++;
	/* All truncations, including zero bytes, are refused. */
	for (i = 0; i < n; i++) assert(bt_media_parse(a, i, &candidate) != 0);
	checks++;
	memcpy(copy, a, sizeof(a)); copy[2] = 0;
	assert(bt_media_parse(copy, n, &candidate) != 0); checks++;
	memcpy(copy, a, sizeof(a)); copy[49] = 1;
	assert(bt_media_parse(copy, n, &candidate) != 0); checks++;
	memcpy(copy, a, sizeof(a)); copy[46] = 1;
	assert(bt_media_parse(copy, n, &candidate) != 0); checks++;
	memcpy(copy, a, sizeof(a)); copy[30] = 0;
	assert(bt_media_parse(copy, n, &candidate) != 0); checks++;
	assert(bt_media_parse(a + 6, n - 6, &candidate) != 0); checks++;
	path(b, 1, 2, UINT64_MAX - 10, 11, 2);
	assert(bt_media_parse(b, n, &candidate) != 0); checks++;
	path(b, 1, 2, 8192, 0, 2);
	assert(bt_media_parse(b, n, &candidate) != 0); checks++;
	/* A complete loaded-image filename is allowed after the GPT node. */
	memcpy(copy, a, sizeof(a));
	copy[48] = 4; copy[49] = 4; copy[50] = 8;
	copy[52] = 'x'; copy[53] = 0; copy[54] = 0; copy[55] = 0;
	copy[56] = 0x7f; copy[57] = 0xff; copy[58] = 4;
	assert(bt_media_parse(copy, 60, &candidate) == 0); checks++;
	copy[54] = 'x';
	assert(bt_media_parse(copy, 60, &candidate) != 0); checks++;
	memset(oversized, 0, sizeof(oversized));
	assert(bt_media_parse(oversized, sizeof(oversized), &candidate) != 0);
	assert(bt_media_parse(NULL, n, &candidate) != 0);
	assert(bt_media_parse(a, n, NULL) != 0); checks++;
	/* Node budget is independent of byte budget. */
	for (i = 0; i < 65; i++) {
		oversized[i * 4] = 1; oversized[i * 4 + 2] = 4;
	}
	memcpy(oversized + 65 * 4, a + 6, n - 6);
	assert(bt_media_parse(oversized, 65 * 4 + n - 6, &candidate) != 0);
	checks++;
	printf("boottrace media parser: %u controls passed\n", checks);
	return (0);
}
