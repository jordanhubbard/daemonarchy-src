/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Jordan Hubbard
 */
#ifndef _SDHCI_BCM2712_POLICY_H_
#define _SDHCI_BCM2712_POLICY_H_

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stdint.h>
#endif

#define BCM2712_CFG_CTRL			0x000
#define BCM2712_CFG_MAX_50MHZ		0x1ac
#define BCM2712_CFG_MIN_SIZE		0x1b0
#define BCM2712_BOOTSTRAP_MAX_HZ		50000000U

/* BCM2712 CFG register fields; see the brcm,sdhci-brcmstb binding and
 * the BCM2712-specific cfginit in Linux v6.12 sdhci-brcmstb.c.
 */
static inline uint32_t
bcm2712_sdhci_50mhz_mode(uint32_t original)
{

	return (original | (1U << 31) | 1U);
}

static inline int
bcm2712_sdhci_is_50mhz(uint32_t value)
{

	return ((value & 0x80000001U) == 0x80000001U);
}

static inline uint32_t
bcm2712_sdhci_force_present(uint32_t original)
{

	return ((original | (1U << 31)) & ~(1U << 30));
}

static inline uint32_t
bcm2712_sdhci_clock(uint32_t requested, uint32_t limit)
{

	if (limit > BCM2712_BOOTSTRAP_MAX_HZ)
		limit = BCM2712_BOOTSTRAP_MAX_HZ;
	return (requested < limit ? requested : limit);
}

#endif
