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
#ifdef _KERNEL
#include <sys/param.h>
#else
#include <stddef.h>
#endif
#include <sys/errno.h>
#include <arm64/broadcom/bcm2712_dma_ram.h>

#define RAM_SIZE (UINT64_C(1) << 36)

/* Bounded admission for the enrolled BCM2712 root/RP1 RAM shapes.  The
 * general policy retains every alias; this backend picks one affine alias
 * for the entire tag.  All admitted offsets preserve page alignment and
 * every admitted power-of-two segment boundary.
 */
int
bcm2712_dma_ram_select(const struct bcm2712_dma_policy *policy, uint64_t mask,
    uint64_t parent_limit, uint64_t alignment, uint64_t boundary,
    uint64_t page_size, struct bcm2712_dma_ram *out)
{
	struct bcm2712_dma_constraints c;
	struct bcm2712_dma_eligible_window eligible[BCM2712_DMA_MAX_RANGES];
	struct bcm2712_dma_ram result;
	unsigned i, count, ram_count;
	int error;

	if (out == NULL || policy == NULL || page_size == 0 ||
	    (page_size & (page_size - 1)) != 0 || page_size > RAM_SIZE ||
	    alignment == 0 || alignment > page_size ||
	    (mask & (page_size - 1)) != page_size - 1 ||
	    (parent_limit & (page_size - 1)) != page_size - 1 ||
	    (boundary != 0 && (boundary > RAM_SIZE ||
	    (boundary & (boundary - 1)) != 0)))
		return (EOPNOTSUPP);
	c = (struct bcm2712_dma_constraints){ mask, alignment, boundary,
	    UINT64_MAX, 1, BCM2712_DMA_RAM };
	error = bcm2712_dma_policy_eligible(policy, &c, eligible,
	    BCM2712_DMA_MAX_RANGES, &count);
	if (error != 0)
		return (error);
	ram_count = 0;
	for (i = 0; i < policy->count; i++) {
		if (policy->ranges[i].role != BCM2712_DMA_RAM)
			continue;
		if (++ram_count > 2 || policy->ranges[i].cpu_base != 0 ||
		    policy->ranges[i].length != RAM_SIZE ||
		    (policy->ranges[i].device_base != 0 &&
		    policy->ranges[i].device_base != RAM_SIZE))
			return (EOPNOTSUPP);
	}
	if (count == 0)
		return (EFBIG);
	/* Eligibility is in declared order.  This profile has only prefixes,
	 * so the first eligible alias covers every physical byte any later
	 * admitted alias could cover under the same mask and parent limit.
	 */
	result.offset = eligible[0].device_base;
	result.physical_limit = eligible[0].length - 1;
	if (result.physical_limit > parent_limit)
		result.physical_limit = parent_limit;
	result.range_index = eligible[0].range_index;
	*out = result;
	return (0);
}

int
bcm2712_dma_ram_address(const struct bcm2712_dma_ram *ram, uint64_t physical,
    uint64_t length, uint64_t *device)
{
	if (ram == NULL || device == NULL || length == 0 ||
	    physical > ram->physical_limit ||
	    length - 1 > ram->physical_limit - physical ||
	    physical > UINT64_MAX - ram->offset ||
	    length - 1 > UINT64_MAX - (physical + ram->offset))
		return (EFBIG);
	*device = physical + ram->offset;
	return (0);
}
