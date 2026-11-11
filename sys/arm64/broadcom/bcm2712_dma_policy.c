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
#include "bcm2712_dma_policy.h"

static int
interval_valid(uint64_t base, uint64_t length)
{
	return (length != 0 && length - 1 <= UINT64_MAX - base);
}

static int
role_valid(enum bcm2712_dma_role role)
{
	return (role == BCM2712_DMA_RAM || role == BCM2712_DMA_PEER);
}

static int
space_valid(uint32_t flags)
{
	uint32_t space = flags & UINT32_C(0x03000000);
	return ((flags & ~UINT32_C(0x43000000)) == 0 &&
	    (space == UINT32_C(0x02000000) || space == UINT32_C(0x03000000)));
}

static int
interval_overlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
	return (a <= b + (blen - 1) && b <= a + (alen - 1));
}

/* Device-space overlap is ambiguous. Physical overlap with the same role is
 * an alias and must survive; physical overlap with conflicting roles is invalid.
 */
static int
policy_valid(const struct bcm2712_dma_policy *policy)
{
	const struct bcm2712_dma_range *a, *b;
	unsigned i, j;

	if (policy == NULL || policy->count == 0 ||
	    policy->count > BCM2712_DMA_MAX_RANGES)
		return (0);
	for (i = 0; i < policy->count; i++) {
		a = &policy->ranges[i];
		if (!interval_valid(a->cpu_base, a->length) ||
		    !interval_valid(a->device_base, a->length) ||
		    !role_valid(a->role) || !space_valid(a->root_space_flags) ||
		    !space_valid(a->child_space_flags))
			return (0);
		for (j = 0; j < i; j++) {
			b = &policy->ranges[j];
			if (interval_overlap(a->device_base, a->length,
			    b->device_base, b->length) ||
			    (a->role != b->role && interval_overlap(a->cpu_base,
			    a->length, b->cpu_base, b->length)))
				return (0);
		}
	}
	return (1);
}

static int
power_two(uint64_t value)
{
	return (value != 0 && (value & (value - 1)) == 0);
}

static int
constraints_valid(const struct bcm2712_dma_constraints *c)
{
	if (c == NULL || !role_valid(c->role) || !power_two(c->alignment) ||
	    (c->boundary != 0 && !power_two(c->boundary)) ||
	    c->max_segment_length == 0 || c->max_segments == 0 ||
	    c->max_segments > BCM2712_DMA_MAX_SEGMENTS)
		return (0);
	/* The inclusive mask contains contiguous low bits, including all64 bits. */
	return (c->device_mask == UINT64_MAX ||
	    power_two(c->device_mask + 1));
}

/* (a-b) modulo a validated power-of-two modulus, without an intermediate
 * unsigned underflow. This also admits unsigned-overflow sanitizer controls.
 */
static uint64_t
mod_difference(uint64_t a, uint64_t b, uint64_t modulus)
{
	a &= modulus - 1;
	b &= modulus - 1;
	return (a >= b ? a - b : modulus - (b - a));
}

int
bcm2712_dma_policy_root(struct bcm2712_dma_policy *out,
    const struct bcm2712_dma_root_window *input, unsigned count)
{
	struct bcm2712_dma_policy stage = {0};
	unsigned i;

	if (out == NULL || input == NULL || count == 0 ||
	    count > BCM2712_DMA_MAX_RANGES)
		return (EINVAL);
	stage.count = count;
	for (i = 0; i < count; i++) {
		stage.ranges[i] = (struct bcm2712_dma_range) {
		    .cpu_base = input[i].cpu_base,
		    .device_base = input[i].parent_bus_base,
		    .length = input[i].length, .role = input[i].role,
		    .root_space_flags = input[i].space_flags,
		    .child_space_flags = input[i].space_flags,
		    .root_index = i, .parent_index = ~0U, .child_index = ~0U };
	}
	if (!policy_valid(&stage))
		return (EINVAL);
	*out = stage;
	return (0);
}

int
bcm2712_dma_policy_compose(struct bcm2712_dma_policy *out,
    const struct bcm2712_dma_policy *parent,
    const struct bcm2712_dma_child_window *input, unsigned count)
{
	struct bcm2712_dma_policy stage = {0};
	const struct bcm2712_dma_range *p;
	const struct bcm2712_dma_child_window *c;
	uint64_t first, last, p_last, c_last;
	unsigned i, j;

	if (out == NULL || !policy_valid(parent) || input == NULL ||
	    count == 0 || count > BCM2712_DMA_MAX_RANGES)
		return (EINVAL);
	/* Validate every child tuple, even one that will have no intersection. */
	for (i = 0; i < count; i++) {
		c = &input[i];
		if (!interval_valid(c->child_bus_base, c->length) ||
		    !interval_valid(c->parent_bus_base, c->length) ||
		    !space_valid(c->parent_space_flags))
			return (EINVAL);
		for (j = 0; j < i; j++)
			if (interval_overlap(c->child_bus_base, c->length,
			    input[j].child_bus_base, input[j].length))
				return (EINVAL);
	}
	/* Child tuple order then parent order is the stable alias order. The
	 * MEM32 annotation does not truncate a parent64-bit address or prevent
	 * composition with a MEM64 parent window.
	 */
	for (i = 0; i < count; i++) {
		c = &input[i];
		c_last = c->parent_bus_base + (c->length - 1);
		for (j = 0; j < parent->count; j++) {
			p = &parent->ranges[j];
			p_last = p->device_base + (p->length - 1);
			first = c->parent_bus_base > p->device_base ?
			    c->parent_bus_base : p->device_base;
			last = c_last < p_last ? c_last : p_last;
			if (first > last)
				continue;
			if (stage.count == BCM2712_DMA_MAX_RANGES)
				return (EFBIG);
			stage.ranges[stage.count++] = (struct bcm2712_dma_range) {
			    .cpu_base = p->cpu_base + (first - p->device_base),
			    .device_base = c->child_bus_base + (first - c->parent_bus_base),
			    .length = last - first + 1, .role = p->role,
			    .root_space_flags = p->root_space_flags,
			    .child_space_flags = c->parent_space_flags,
			    .root_index = p->root_index, .parent_index = j,
			    .child_index = i };
		}
	}
	if (stage.count == 0)
		return (ENXIO);
	if (!policy_valid(&stage))
		return (EINVAL);
	*out = stage;
	return (0);
}

/* Returns a byte preimage, retaining congruence rather than rounding away
 * bytes which can still be part of a legal aligned-start transfer.
 */
static int
eligible_range(const struct bcm2712_dma_range *r,
    const struct bcm2712_dma_constraints *c, unsigned index,
    struct bcm2712_dma_eligible_window *out)
{
	uint64_t length;

	if (r->role != c->role || r->device_base > c->device_mask)
		return (0);
	length = r->length;
	if (length - 1 > c->device_mask - r->device_base)
		length = c->device_mask - r->device_base + 1;
	*out = (struct bcm2712_dma_eligible_window) {
	    .cpu_base = r->cpu_base, .device_base = r->device_base,
	    .length = length, .start_modulus = c->alignment,
	    .start_remainder = mod_difference(r->cpu_base, r->device_base, c->alignment),
	    .boundary = c->boundary, .max_segment_length = c->max_segment_length,
	    .range_index = index };
	return (1);
}

int
bcm2712_dma_policy_eligible(const struct bcm2712_dma_policy *policy,
    const struct bcm2712_dma_constraints *c,
    struct bcm2712_dma_eligible_window *out, unsigned capacity, unsigned *count)
{
	struct bcm2712_dma_eligible_window stage[BCM2712_DMA_MAX_RANGES];
	unsigned i, n = 0;

	if (!policy_valid(policy) || !constraints_valid(c) || count == NULL ||
	    (out == NULL && capacity != 0))
		return (EINVAL);
	for (i = 0; i < policy->count; i++)
		if (eligible_range(&policy->ranges[i], c, i, &stage[n]))
			n++;
	if (n > capacity)
		return (EFBIG);
	for (i = 0; i < n; i++)
		out[i] = stage[i];
	*count = n;
	return (0);
}

/* Check physical coverage without constraints so a real hole is ENXIO,
 * distinct from a covered physical span which needs bouncing (EFBIG).
 */
static int
span_covered(const struct bcm2712_dma_policy *policy,
    enum bcm2712_dma_role role, uint64_t cpu, uint64_t length)
{
	uint64_t cover, offset, part;
	unsigned i;

	while (length != 0) {
		cover = 0;
		for (i = 0; i < policy->count; i++) {
			const struct bcm2712_dma_range *r = &policy->ranges[i];
			if (r->role != role || cpu < r->cpu_base)
				continue;
			offset = cpu - r->cpu_base;
			if (offset >= r->length)
				continue;
			part = r->length - offset;
			if (part > cover)
				cover = part;
		}
		if (cover == 0)
			return (0);
		if (cover >= length)
			return (1);
		cpu += cover;
		length -= cover;
	}
	return (1);
}

/* Latest positive endpoint that can begin a next eligible aligned alias.
 * Align the NEXT domain, not the length of the CURRENT segment: adjacent
 * aliases can have different alignment congruences.
 */
static uint64_t
continuation_prefix(const struct bcm2712_dma_policy *policy,
    const struct bcm2712_dma_constraints *c, uint64_t cpu, uint64_t prefix)
{
	struct bcm2712_dma_eligible_window next;
	uint64_t limit, last, delta, end, best = 0;
	unsigned i;

	end = cpu + prefix; /* Nonfinal prefix: at most the last input byte. */
	for (i = 0; i < policy->count; i++) {
		if (!eligible_range(&policy->ranges[i], c, i, &next))
			continue;
		last = next.cpu_base + (next.length - 1);
		limit = end < last ? end : last;
		delta = mod_difference(limit, next.start_remainder, next.start_modulus);
		if (limit < delta)
			continue;
		limit -= delta;
		if (limit > cpu && limit >= next.cpu_base && limit - cpu > best)
			best = limit - cpu;
	}
	return (best);
}

static int
can_merge(const struct bcm2712_dma_device_segment *a,
    const struct bcm2712_dma_device_segment *b,
    const struct bcm2712_dma_constraints *c)
{
	uint64_t total;

	if (a->range_index != b->range_index ||
	    a->length > UINT64_MAX - a->cpu_base ||
	    a->length > UINT64_MAX - a->device_base ||
	    a->cpu_base + a->length != b->cpu_base ||
	    a->device_base + a->length != b->device_base ||
	    b->length > c->max_segment_length - a->length)
		return (0);
	total = a->length + b->length;
	return (c->boundary == 0 ||
	    (a->device_base & ~(c->boundary - 1)) ==
	    ((a->device_base + (total - 1)) & ~(c->boundary - 1)));
}

int
bcm2712_dma_policy_map(const struct bcm2712_dma_policy *policy,
    const struct bcm2712_dma_constraints *c,
    const struct bcm2712_dma_cpu_span *input, unsigned ninput,
    struct bcm2712_dma_device_segment *out, unsigned capacity, unsigned *count)
{
	struct bcm2712_dma_device_segment stage[BCM2712_DMA_MAX_SEGMENTS], piece;
	struct bcm2712_dma_eligible_window e;
	uint64_t cpu, remaining, offset, device, prefix, best;
	unsigned i, j, n = 0, selected;

	if (!policy_valid(policy) || !constraints_valid(c) || count == NULL ||
	    (input == NULL && ninput != 0) || ninput > BCM2712_DMA_MAX_SEGMENTS ||
	    (out == NULL && capacity != 0))
		return (EINVAL);
	for (i = 0; i < ninput; i++) {
		if (!interval_valid(input[i].cpu_base, input[i].length))
			return (EINVAL);
	}
	for (i = 0; i < ninput; i++)
		if (!span_covered(policy, c->role, input[i].cpu_base, input[i].length))
			return (ENXIO);
	for (i = 0; i < ninput; i++) {
		cpu = input[i].cpu_base;
		remaining = input[i].length;
		while (remaining != 0) {
			best = 0;
			selected = 0;
			for (j = 0; j < policy->count; j++) {
				if (!eligible_range(&policy->ranges[j], c, j, &e) ||
				    cpu < e.cpu_base)
					continue;
				offset = cpu - e.cpu_base;
				if (offset >= e.length)
					continue;
				device = e.device_base + offset;
				if ((device & (c->alignment - 1)) != 0)
					continue;
				prefix = e.length - offset;
				if (prefix > remaining)
					prefix = remaining;
				if (prefix > c->max_segment_length)
					prefix = c->max_segment_length;
				if (c->boundary != 0 && prefix >
				    c->boundary - (device & (c->boundary - 1)))
					prefix = c->boundary - (device & (c->boundary - 1));
				if (prefix < remaining)
					prefix = continuation_prefix(policy, c, cpu, prefix);
				if (prefix > best) {
					best = prefix;
					selected = j;
				}
			}
			if (best == 0)
				return (EFBIG);
			piece = (struct bcm2712_dma_device_segment) {
			    .cpu_base = cpu, .device_base = policy->ranges[selected].device_base +
			    (cpu - policy->ranges[selected].cpu_base),
			    .length = best, .range_index = selected };
			if (n != 0 && can_merge(&stage[n - 1], &piece, c))
				stage[n - 1].length += best;
			else {
				if (n == c->max_segments || n == capacity)
					return (EFBIG);
				stage[n++] = piece;
			}
			remaining -= best;
			if (remaining != 0)
				cpu += best;
		}
	}
	for (i = 0; i < n; i++)
		out[i] = stage[i];
	*count = n;
	return (0);
}
