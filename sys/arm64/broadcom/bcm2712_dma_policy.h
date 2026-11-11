/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jordan Hubbard
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#ifndef _BCM2712_DMA_POLICY_H_
#define _BCM2712_DMA_POLICY_H_
#include <sys/types.h>
#include <sys/stdint.h>

#define BCM2712_DMA_MAX_RANGES 32
#define BCM2712_DMA_MAX_SEGMENTS 64

enum bcm2712_dma_role { BCM2712_DMA_RAM = 1, BCM2712_DMA_PEER = 2 };
/* Raw MSI addresses have a different type and no data-translation operation. */
struct bcm2712_dma_msi_route {
	uint64_t pci_message_addr, cpu_doorbell_addr, length;
};
struct bcm2712_dma_root_window {
	uint64_t cpu_base, parent_bus_base, length;
	enum bcm2712_dma_role role;
	uint32_t space_flags;
};
struct bcm2712_dma_child_window {
	uint64_t child_bus_base, parent_bus_base, length;
	uint32_t parent_space_flags;
};
struct bcm2712_dma_range {
	uint64_t cpu_base, device_base, length;
	enum bcm2712_dma_role role;
	uint32_t root_space_flags, child_space_flags;
	unsigned root_index, parent_index, child_index;
};
/* Construct unpublished, then treat as an immutable snapshot. No MMIO,
 * allocation, backend, callback or live-consumer publication occurs here.
 */
struct bcm2712_dma_policy {
	unsigned count;
	struct bcm2712_dma_range ranges[BCM2712_DMA_MAX_RANGES];
};
struct bcm2712_dma_constraints {
	uint64_t device_mask, alignment, boundary, max_segment_length;
	unsigned max_segments;
	enum bcm2712_dma_role role;
};
struct bcm2712_dma_cpu_span { uint64_t cpu_base, length; };
struct bcm2712_dma_device_segment {
	uint64_t cpu_base, device_base, length;
	unsigned range_index;
};
/* Byte preimage plus start congruence. A candidate start p must satisfy
 * p % start_modulus == start_remainder, and the whole requested span must
 * satisfy the reported boundary/max-segment and interval limits.
 */
struct bcm2712_dma_eligible_window {
	uint64_t cpu_base, device_base, length;
	uint64_t start_modulus, start_remainder, boundary, max_segment_length;
	unsigned range_index;
};

/* Success replaces the output. Failure leaves all output bytes/counts intact.
 * Intervals use inclusive last-byte validation: MAX,length1 is valid; length0
 * and wrapping last-byte intervals are invalid. CPU aliases are retained;
 * overlapping device-domain windows or conflicting physical roles are invalid.
 */
int bcm2712_dma_policy_root(struct bcm2712_dma_policy *,
    const struct bcm2712_dma_root_window *, unsigned);
int bcm2712_dma_policy_compose(struct bcm2712_dma_policy *,
    const struct bcm2712_dma_policy *,
    const struct bcm2712_dma_child_window *, unsigned);
int bcm2712_dma_policy_eligible(const struct bcm2712_dma_policy *,
    const struct bcm2712_dma_constraints *,
    struct bcm2712_dma_eligible_window *, unsigned, unsigned *);
/* Physical holes are ENXIO; covered bytes excluded by constraints/budget are
 * EFBIG. Empty input succeeds; individual zero-length spans are EINVAL.
 * Longest eligible prefix wins, declared range order breaks ties. A nonfinal
 * prefix must end at an eligible aligned start in some alias. This is a bounded
 * deterministic policy, not a globally minimum-segment optimizer. A greedy
 * dead end can be EFBIG even when a different legal cover exists; it is a
 * conservative direct-map admission, not proof that no mapping is possible.
 */
int bcm2712_dma_policy_map(const struct bcm2712_dma_policy *,
    const struct bcm2712_dma_constraints *,
    const struct bcm2712_dma_cpu_span *, unsigned,
    struct bcm2712_dma_device_segment *, unsigned, unsigned *);
#endif
