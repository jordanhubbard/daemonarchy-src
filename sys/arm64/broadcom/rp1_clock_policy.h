/* $OpenBSD: rpiclock.c,v 1.1 2025/09/17 09:23:43 kettenis Exp $ */

/*
 * Copyright (c) 2025 Marcus Glocker <mglocker@openbsd.org>
 * Copyright (c) 2025 Mark Kettenis <kettenis@openbsd.org>
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


#ifndef _ARM64_BROADCOM_RP1_CLOCK_POLICY_H_
#define _ARM64_BROADCOM_RP1_CLOCK_POLICY_H_
#include <sys/types.h>
#include <sys/errno.h>

enum rp1_clock_kind { RP1_CORE, RP1_PRIMARY, RP1_SECONDARY, RP1_HALF, RP1_LEAF };
struct rp1_clock_rate {
	uint32_t value, extra;
	uint64_t actual;
};

static inline int
rp1_clock_allowed(uint64_t requested, uint64_t actual, int flags)
{

	if (requested == actual)
		return (1);
	return (actual > requested ? (flags & CLK_SET_ROUND_UP) != 0 :
	    (flags & CLK_SET_ROUND_DOWN) != 0);
}

static inline uint64_t
rp1_clock_distance(uint64_t a, uint64_t b)
{

	return (a > b ? a - b : b - a);
}

/* No register access: rejected or dry-run requests cannot change hardware. */
static inline int
rp1_clock_rate_plan(enum rp1_clock_kind kind, uint64_t fin, uint64_t requested,
    int flags, uint32_t maximum, struct rp1_clock_rate *rate)
{
	struct rp1_clock_rate candidate;
	uint64_t scaled, divider, actual, distance = UINT64_MAX;
	unsigned d, e, first, last;
	int found = 0;

	if (fin == 0 || fin > UINT64_C(4000000000) || requested == 0 ||
	    requested > UINT64_C(2000000000))
		return (ERANGE);
	if ((flags & ~(CLK_SET_ROUND_UP | CLK_SET_ROUND_DOWN | CLK_SET_DRYRUN)) != 0)
		return (EOPNOTSUPP);
	if (kind == RP1_CORE) {
		/* Selected SYS/AUDIO VCO profile; 24 fractional feedback bits. */
		if (fin > UINT64_C(100000000) || fin > requested / 16)
			return (ERANGE);
		scaled = ((requested << 24) + fin / 2) / fin;
		if (scaled / (1U << 24) > 0xfff)
			return (ERANGE);
		actual = (fin * scaled + (1U << 23)) >> 24;
		if (actual < requested && (flags & CLK_SET_ROUND_UP) != 0 &&
		    (flags & CLK_SET_ROUND_DOWN) == 0)
			scaled++;
		if (actual > requested && (flags & CLK_SET_ROUND_DOWN) != 0 &&
		    (flags & CLK_SET_ROUND_UP) == 0)
			scaled--;
		actual = (fin * scaled + (1U << 23)) >> 24;
		if (!rp1_clock_allowed(requested, actual, flags))
			return (ERANGE);
		*rate = (struct rp1_clock_rate) {
		    .value = scaled >> 24, .extra = scaled & 0xffffff, .actual = actual };
		return (0);
	}
	if (kind == RP1_HALF) {
		actual = (fin + 1) / 2;
		if (!rp1_clock_allowed(requested, actual, flags))
			return (ERANGE);
		*rate = (struct rp1_clock_rate) { .actual = actual };
		return (0);
	}
	if (kind == RP1_LEAF) {
		if (maximum == 0)
			return (EINVAL);
		/* Only the floor and ceil of the ideal divider can be closest. */
		divider = fin / requested;
		for (unsigned i = 0; i < 2; i++) {
			uint64_t div = divider + i;
			if (div < 1)
				div = 1;
			if (div > maximum)
				div = maximum;
			actual = fin / div;
			if (!rp1_clock_allowed(requested, actual, flags) || actual == 0)
				continue;
			if (!found || rp1_clock_distance(actual, requested) < distance) {
				*rate = (struct rp1_clock_rate) { .value = div, .actual = actual };
				distance = rp1_clock_distance(actual, requested);
				found = 1;
			}
		}
		return (found ? 0 : ERANGE);
	}
	if (kind != RP1_PRIMARY && kind != RP1_SECONDARY)
		return (EINVAL);
	first = kind == RP1_PRIMARY ? 1 : 8;
	last = kind == RP1_PRIMARY ? 7 : 19;
	for (d = first; d <= last; d++) {
		for (e = 1; e <= (kind == RP1_PRIMARY ? d : 1); e++) {
			actual = (fin + (d * e) / 2) / (d * e);
			if (!rp1_clock_allowed(requested, actual, flags))
				continue;
			candidate = (struct rp1_clock_rate) { .value = d, .extra = e, .actual = actual };
			if (!found || rp1_clock_distance(actual, requested) < distance) {
				*rate = candidate;
				distance = rp1_clock_distance(actual, requested);
				found = 1;
			}
		}
	}
	return (found ? 0 : ERANGE);
}
#endif
