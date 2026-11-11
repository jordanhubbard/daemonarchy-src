/*
 * Copyright (c) 2020, 2025 Mark Kettenis <kettenis@openbsd.org>
 * Copyright (c) 2026 Jordan Hubbard
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _BCM2712_PCIE_HW_H_
#define _BCM2712_PCIE_HW_H_
#include <sys/types.h>
#include <sys/stdint.h>
#include <arm64/broadcom/bcm2712_dma_policy.h>

/* Host-order cells from the complete selected port2 properties. The wrapper
 * validates string/boolean/absence properties and provider node identities. */
struct bcm2712_pcie_dt {
	const uint32_t *reg, *ranges, *dma, *resets, *interrupts, *interrupt_map;
	unsigned nreg, nranges, ndma, nresets, ninterrupts, ninterrupt_map;
	uint32_t parent_acells, parent_scells, acells, scells, icells;
	uint32_t domain, lanes, generation, msi_parent, interrupt_parent;
	uint32_t interrupt_mask[4];
	uint8_t qos[8];
	/* Nonzero means an unsupported property/profile was observed. */
	unsigned unsupported;
};
int bcm2712_pcie_config_address(unsigned, unsigned, unsigned, unsigned,
    unsigned, uint32_t *, uint32_t *);
struct bcm2712_pcie_window { uint64_t pci, cpu, length; };
struct bcm2712_pcie_outbound {
	uint32_t pci_lo, pci_hi, base_limit, base_hi, limit_hi;
	uint64_t decode_first, decode_last;
};
struct bcm2712_pcie_inbound { uint32_t pci_lo, pci_hi, cpu_lo, cpu_hi; };
struct bcm2712_pcie_plan {
	struct bcm2712_pcie_window outbound[2], inbound[3];
	struct bcm2712_pcie_outbound out[4];
	struct bcm2712_pcie_inbound in[10];
	struct bcm2712_dma_policy ram_policy;
	struct bcm2712_dma_msi_route raw_msi;
	uint32_t qos, validated;
};
/* No failed constructor modifies its output. No allocation or MMIO. */
int bcm2712_pcie_encode_outbound(const struct bcm2712_pcie_window *,
    struct bcm2712_pcie_outbound *);
int bcm2712_pcie_encode_inbound(const struct bcm2712_pcie_window *,
    struct bcm2712_pcie_inbound *);
int bcm2712_pcie_plan_make(const struct bcm2712_pcie_dt *,
    struct bcm2712_pcie_plan *);

/* Original checked FreeBSD sequencing, adapted from the ISC OpenBSD hardware
 * operations. Linux is a pinned factual reference, not an imported driver. */
enum bcm2712_pcie_reset { BCM2712_PCIE_BRIDGE, BCM2712_PCIE_RESCAL };
struct bcm2712_pcie_ops {
	int (*read)(void *, uint32_t, unsigned, uint32_t *);
	int (*write)(void *, uint32_t, unsigned, uint32_t);
	int (*reset)(void *, enum bcm2712_pcie_reset, int);
	void (*delay)(void *, unsigned);
	int (*dependencies)(void *);
	int (*publish)(void *);
};
struct bcm2712_pcie_state {
	unsigned phase, generation, lanes;
	int error;
	unsigned touched, link, publication_attempted, published, retained;
	unsigned qos_fallback;
};
/* A failed touched state is retained; no pulse retry or reverse reset is
 * assumed. The first frontend refuses detach/power transitions. */
int bcm2712_pcie_start(const struct bcm2712_pcie_plan *,
    const struct bcm2712_pcie_ops *, void *, struct bcm2712_pcie_state *);
#endif
