/* SPDX-License-Identifier: BSD-2-Clause */
/* Register-field and card-clock controls; not controller/driver acceptance. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "sdhci_bcm2712_policy.h"

int
main(void)
{
	static const struct {
		uint32_t original, mode, present;
	} registers[] = {
		{ 0x00000000, 0x80000001, 0x80000000 },
		{ 0x40000000, 0xc0000001, 0x80000000 },
		{ 0xffffffff, 0xffffffff, 0xbfffffff },
		{ 0x12345678, 0x92345679, 0x92345678 },
	};
	static const struct {
		uint32_t requested, limit, expected;
	} clocks[] = {
		{ 0, 50000000, 0 },
		{ 400000, 50000000, 400000 },
		{ 25000000, 50000000, 25000000 },
		{ 50000000, 50000000, 50000000 },
		{ 50000001, 50000000, 50000000 },
		{ 200000000, 50000000, 50000000 },
		{ UINT32_MAX, UINT32_MAX, 50000000 },
		{ 50000000, 25000000, 25000000 },
		{ 400000, 0, 0 },
	};
	uint32_t original;
	size_t i;

	for (i = 0; i < sizeof(registers) / sizeof(registers[0]); i++) {
		original = registers[i].original;
		assert(bcm2712_sdhci_50mhz_mode(original) == registers[i].mode);
		assert(bcm2712_sdhci_is_50mhz(registers[i].mode));
		assert(bcm2712_sdhci_force_present(original) == registers[i].present);
		/* Non-owned register fields must survive either operation. */
		assert(((bcm2712_sdhci_50mhz_mode(original) ^ original) &
		    0x7ffffffe) == 0);
		assert(((bcm2712_sdhci_force_present(original) ^ original) &
		    0x3fffffff) == 0);
	}
	assert(!bcm2712_sdhci_is_50mhz(0x00000000));
	assert(!bcm2712_sdhci_is_50mhz(0x00000001));
	assert(!bcm2712_sdhci_is_50mhz(0x80000000));
	for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++)
		assert(bcm2712_sdhci_clock(clocks[i].requested, clocks[i].limit) ==
		    clocks[i].expected);
	puts("PASS: BCM2712 CFG field preservation, forced-presence polarity, card-clock stop/identification/50MHz limits");
	return (0);
}
