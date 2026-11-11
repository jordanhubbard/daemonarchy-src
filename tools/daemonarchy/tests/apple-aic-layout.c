/* SPDX-License-Identifier: BSD-2-Clause */
/* Board-derived IRQ examples and malformed firmware boundary controls. */
#include <assert.h>
#include <stdio.h>

#include "../../../sys/arm64/apple/apple_aic_layout.h"

int
main(void)
{
	struct apple_aic_layout layout;
	uint32_t die, irq;
	uint32_t uart[] = { 0, 0, 0x4ae, 4 };
	uint32_t nvme[] = { 0, 0, 0x728, 4 };
	uint32_t timer[] = { 1, 0, 3, 4 };
	uint32_t legacy[] = { 0, 895, 4 };
	uint32_t boundary[] = { 0, 1, 2047, 4 };

	/* AICv1 retains its documented fixed 1024-slot register banks. */
	assert(apple_aic_layout(1, 896, 0, 0x8000, &layout) == 0);
	assert(layout.nirqs == 896 && layout.ndie == 1);
	assert(layout.sw_clear == 0x4080 && layout.mask_set == 0x4100);
	assert(layout.mask_clear == 0x4180 && layout.die_stride == 0x1280);
	assert(apple_aic_decode(&layout, 3, legacy, &die, &irq) == 0);
	assert(die == 0 && irq == 895);
	legacy[1]++;
	assert(apple_aic_decode(&layout, 3, legacy, &die, &irq) == EINVAL);
	assert(apple_aic_layout(1, 1025, 0, 0x8000, &layout) == EINVAL);
	assert(apple_aic_layout(1, 0x01000380, 0, 0x8000, &layout) == EINVAL);

	/* 2048-slot AICv2 fixture covers J474s UART/NVMe and timer specifiers.
	 * The specifiers come from the published J474s/T6020 DTB; register INFO
	 * values are fixtures, not measurements of a running Mac controller. */
	assert(apple_aic_layout(2, 2048, 0x01000800, 0xc000, &layout) == 0);
	assert(layout.sw_clear == 0x4100 && layout.mask_set == 0x4200);
	assert(layout.mask_clear == 0x4300 && layout.die_stride == 0x2500);
	assert(apple_aic_decode(&layout, 4, uart, &die, &irq) == 0);
	assert(die == 0 && irq == 0x4ae);
	assert(layout.mask_clear + (irq >> 5) * 4 == 0x4394);
	assert(apple_aic_decode(&layout, 4, nvme, &die, &irq) == 0);
	assert(die == 0 && irq == 0x728);
	assert(apple_aic_decode(&layout, 4, timer, &die, &irq) == 0);
	assert(die == 0 && irq == 3);
	timer[2] = 4;
	assert(apple_aic_decode(&layout, 4, timer, &die, &irq) == EINVAL);
	timer[2] = 0;
	timer[1] = 1;
	assert(apple_aic_decode(&layout, 4, timer, &die, &irq) == EINVAL);

	/* Two dies: the last IRQ is valid; neither the next IRQ nor die is. */
	assert(apple_aic_layout(2, 0x01000800, 0x02000800, 0xc000, &layout) == 0);
	assert(apple_aic_decode(&layout, 4, boundary, &die, &irq) == 0);
	assert(die == 1 && irq == 2047);
	assert(layout.mask_clear + die * layout.die_stride + (irq >> 5) * 4 == 0x68fc);
	boundary[2]++;
	assert(apple_aic_decode(&layout, 4, boundary, &die, &irq) == EINVAL);
	boundary[2] = 0;
	boundary[1]++;
	assert(apple_aic_decode(&layout, 4, boundary, &die, &irq) == EINVAL);
	boundary[0] = 2;
	boundary[1] = 0;
	assert(apple_aic_decode(&layout, 4, boundary, &die, &irq) == EINVAL);
	assert(apple_aic_decode(&layout, 2, boundary, &die, &irq) == EINVAL);
	assert(apple_aic_decode(&layout, 5, boundary, &die, &irq) == EINVAL);

	/* Refuse invalid register capacities before any driver MMIO writes. */
	assert(apple_aic_layout(2, 0, 0x01000800, 0xc000, &layout) == EINVAL);
	assert(apple_aic_layout(2, 2048, 0x010007ff, 0xc000, &layout) == EINVAL);
	assert(apple_aic_layout(2, 2048, 0x00000800, 0xc000, &layout) == EINVAL);
	assert(apple_aic_layout(2, 0x04000800, 0x05000800, 0x20000, &layout) == EINVAL);
	assert(apple_aic_layout(2, 0x01000800, 0x01000800, 0xc000, &layout) == EINVAL);
	assert(apple_aic_layout(2, 2048, 0x01000800, 0x44ff, &layout) == EINVAL);
	assert(apple_aic_layout(3, 2048, 0x01000800, 0x20000, &layout) == EINVAL);
	puts("PASS: AICv1/v2 layout, J474s IRQs, die/FIQ/capacity bounds");
	return (0);
}
