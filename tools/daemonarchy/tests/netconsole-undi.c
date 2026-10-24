/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "netconsole_undi.h"

static uint8_t rom[36];
static struct nc_undi_info info;
static struct nc_undi_region regions[2];

static void
put64(size_t offset, uint64_t value)
{
	unsigned i;
	for (i = 0; i < 8; i++)
		rom[offset + i] = value >> (8 * i);
}

static void
checksum(void)
{
	uint8_t sum = 0;
	unsigned i;
	rom[5] = 0;
	for (i = 0; i < sizeof(rom); i++)
		sum += rom[i];
	rom[5] = -sum;
}

static void
reset(void)
{
	memset(rom, 0, sizeof(rom));
	memcpy(rom, "!PXE", 4);
	rom[4] = sizeof(rom);
	rom[6] = 2;
	rom[7] = 1;
	rom[8] = 3;
	rom[9] = 1;
	rom[15] = 0x40;
	put64(16, 0x2100);
	rom[27] = 2;
	memcpy(rom + 28, "USBRPCIR", 8);
	checksum();
	regions[0] = (struct nc_undi_region){0x1000, 0x1000,
	    UINT64_C(0x8000000000000000), 6};
	regions[1] = (struct nc_undi_region){0x2000, 0x1000,
	    UINT64_C(0x8000000000000000), 5};
}

static enum nc_undi_status
inspect(uint64_t address, uint16_t interface)
{
	return (nc_undi_inspect(rom, sizeof(rom), address, interface,
	    regions, 2, &info));
}

int
main(void)
{
	unsigned i;
	reset();
	assert(inspect(0x1000, 1) == NC_UNDI_CANDIDATE);
	assert(info.entry == 0x2100 && info.interface == 1);
	assert(inspect(0x1000, 2) == NC_UNDI_INTERFACE && info.entry == 0);
	rom[10] = 1;
	checksum();
	assert(inspect(0x1000, 257) == NC_UNDI_CANDIDATE);

	/* Relative entry, including addition overflow, is not an absolute pointer. */
	reset();
	rom[15] = 0;
	put64(16, 0x1100);
	checksum();
	assert(inspect(0x1000, 0) == NC_UNDI_CANDIDATE && info.entry == 0x2100);
	assert(inspect(UINT64_MAX - 8, 0) == NC_UNDI_ADDRESS);

	/* Every table truncation refuses; output never leaks a prior success. */
	reset();
	for (i = 0; i < sizeof(rom); i++) {
		info.entry = UINT64_MAX;
		assert(nc_undi_inspect(rom, i, 0x1000, 0, regions, 2,
		    &info) != NC_UNDI_CANDIDATE && info.entry == 0);
	}
	assert(nc_undi_inspect(NULL, 0, 0, 0, regions, 2, &info) == NC_UNDI_FORMAT);
	rom[20] ^= 1;
	assert(inspect(0x1000, 0) == NC_UNDI_CHECKSUM);
	reset();
	rom[9] = 2;
	checksum();
	assert(inspect(0x1000, 0) == NC_UNDI_VERSION);
	reset();
	rom[15] |= 0x80;
	checksum();
	assert(inspect(0x1000, 0) == NC_UNDI_HARDWARE);
	reset();
	memcpy(rom + 32, "1394", 4);
	checksum();
	assert(inspect(0x1000, 0) == NC_UNDI_BUS);
	reset();
	rom[27] = 3;
	checksum();
	assert(inspect(0x1000, 0) == NC_UNDI_FORMAT);
	reset();
	rom[24] = 1;
	checksum();
	assert(inspect(0x1000, 0) == NC_UNDI_FORMAT);
	reset();
	put64(16, 0);
	checksum();
	assert(inspect(0x1000, 0) == NC_UNDI_ADDRESS);

	/* Boot-services code/data, missing runtime attributes, NX and boundary
	 * violations must never be advertised as a runtime candidate.
	 */
	reset();
	regions[0].type = 4;
	assert(inspect(0x1000, 0) == NC_UNDI_TABLE_MEMORY);
	reset();
	regions[0].attributes = 0;
	assert(inspect(0x1000, 0) == NC_UNDI_TABLE_MEMORY);
	reset();
	regions[1].type = 3;
	assert(inspect(0x1000, 0) == NC_UNDI_ENTRY_MEMORY);
	reset();
	regions[1].type = 6;
	assert(inspect(0x1000, 0) == NC_UNDI_ENTRY_MEMORY);
	reset();
	regions[1].attributes |= 0x4000;
	assert(inspect(0x1000, 0) == NC_UNDI_ENTRY_MEMORY);
	reset();
	regions[0].size = sizeof(rom) - 1;
	assert(inspect(0x1000, 0) == NC_UNDI_TABLE_MEMORY);
	reset();
	regions[1].size = 0x100;
	assert(inspect(0x1000, 0) == NC_UNDI_ENTRY_MEMORY);
	reset();
	regions[1].size = UINT64_MAX;
	assert(inspect(0x1000, 0) == NC_UNDI_ENTRY_MEMORY);
	puts("UNDI descriptor admission: all checks passed (not kernel transport acceptance)");
	return (0);
}
