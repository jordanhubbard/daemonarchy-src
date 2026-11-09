/*-
 * SPDX-License-Identifier: BSD-2-Clause AND ISC
 *
 * AICv2 register layout derived from OpenBSD aplintc.c.
 * Copyright (c) 2021 Mark Kettenis
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
#ifndef _APPLE_AIC_LAYOUT_H_
#define _APPLE_AIC_LAYOUT_H_

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stdint.h>
#endif
#include <sys/errno.h>

#define APPLE_AIC_MAXDIES 4
#define APPLE_AIC_NFIQS 4

struct apple_aic_layout {
	uint32_t nirqs;
	uint32_t ndie;
	uint32_t sw_clear;
	uint32_t mask_set;
	uint32_t mask_clear;
	uint32_t die_stride;
};

static inline int
apple_aic_layout(uint32_t version, uint32_t info, uint32_t info3,
    uint64_t size, struct apple_aic_layout *layout)
{
	uint32_t maxirqs, maxdies, base, banks, stride;

	if (version == 1) {
		maxirqs = 1024;
		maxdies = 1;
		base = 0x3000;
	} else if (version == 2) {
		maxirqs = info3 & 0xffff;
		maxdies = (info3 >> 24) & 0xf;
		base = 0x2000;
	} else {
		return (EINVAL);
	}
	layout->nirqs = info & 0xffff;
	layout->ndie = ((info >> 24) & 0xf) + 1;
	if (layout->nirqs == 0 || layout->nirqs > maxirqs ||
	    maxirqs == 0 || (maxirqs & 31) != 0 ||
	    layout->ndie > maxdies || layout->ndie > APPLE_AIC_MAXDIES)
		return (EINVAL);

	/* Per die: IRQ configuration words and five bitmap banks. */
	banks = (maxirqs / 32) * 4;
	stride = maxirqs * 4;
	layout->sw_clear = base + stride + banks;
	layout->mask_set = base + stride + 2 * banks;
	layout->mask_clear = base + stride + 3 * banks;
	layout->die_stride = stride + 5 * banks;
	if (base + layout->ndie * layout->die_stride > size)
		return (EINVAL);
	return (0);
}

static inline int
apple_aic_decode(const struct apple_aic_layout *layout, uint32_t ncells,
    const uint32_t *cells, uint32_t *die, uint32_t *irq)
{
	if (ncells != 3 && ncells != 4)
		return (EINVAL);
	*die = ncells == 4 ? cells[1] : 0;
	*irq = cells[ncells - 2];
	if (cells[0] == 0) {
		if (*die >= layout->ndie || *irq >= layout->nirqs)
			return (EINVAL);
	} else if (cells[0] == 1) {
		if (*die != 0 || *irq >= APPLE_AIC_NFIQS)
			return (EINVAL);
	} else {
		return (EINVAL);
	}
	return (0);
}

#endif /* _APPLE_AIC_LAYOUT_H_ */
