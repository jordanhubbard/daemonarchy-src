/*
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

/* FreeBSD D0 bootstrap adaptation. Register plans are validated before MMIO. */
#ifndef _BCM2712_PINCTRL_POLICY_H_
#define _BCM2712_PINCTRL_POLICY_H_

struct bcmstbpinctrl_pin {
	char name[12];
	uint8_t func_reg, func_off;
	uint8_t bias_reg, bias_off;
	const char *func[10];
};

static const struct bcmstbpinctrl_pin bcmstbpinctrl_d0_pins[] = {
	/* Additional enabled firmware defaults; D0 register facts from Linux. */
	{ "gpio1", 0, 0, 4, 5, { "gpio" } },
	{ "gpio2", 0, 1, 4, 6, { [0] = "gpio", [7] = "vc_spi0" } },
	{ "gpio3", 0, 2, 4, 7, { [0] = "gpio", [6] = "vc_spi0" } },
	{ "gpio4", 0, 3, 4, 8, { [0] = "gpio", [6] = "vc_spi0" } },
	{ "gpio24", 2, 0, 5, 6, { [0] = "gpio", [4] = "uart0" } },
	{ "gpio25", 2, 1, 5, 7, { [0] = "gpio", [4] = "uart0" } },
	{ "gpio26", 2, 2, 5, 8, { [0] = "gpio", [4] = "uart0" } },
	{ "gpio27", 2, 3, 5, 9, { [0] = "gpio", [4] = "uart0" } },
	{ "gpio29", 2, 5, 5, 11, { "gpio" } },
	{ "gpio20", 1, 4, 5, 2, { "gpio" } },
	{ "gpio28", 2, 4, 5, 10, { "gpio" } },
	{ "gpio30", 2, 6, 5, 12, { "gpio", "sd2" } },
	{ "gpio31", 2, 7, 5, 13, { "gpio", "sd2" } },
	{ "gpio32", 3, 0, 5, 14, { "gpio", "sd2" } },
	{ "gpio33", 3, 1, 6, 0, { "gpio", "sd2" } },
	{ "gpio34", 3, 2, 6, 1, { "gpio", "sd2" } },
	{ "gpio35", 3, 3, 6, 2, { "gpio", "sd2" } },
	{ "emmc_cmd", 0, 0, 6, 3 /* no mux */, { NULL } },
	{ "emmc_dat0", 0, 0, 6, 6 /* no mux */, { NULL } },
	{ "emmc_dat1", 0, 0, 6, 7 /* no mux */, { NULL } },
	{ "emmc_dat2", 0, 0, 6, 8 /* no mux */, { NULL } },
	{ "emmc_dat3", 0, 0, 6, 9 /* no mux */, { NULL } },
	{ "emmc_dat4", 0, 0, 6, 10 /* no mux */, { NULL } },
	{ "emmc_dat5", 0, 0, 6, 11 /* no mux */, { NULL } },
	{ "emmc_dat6", 0, 0, 6, 12 /* no mux */, { NULL } },
	{ "emmc_dat7", 0, 0, 6, 13 /* no mux */, { NULL } },
};

static const struct bcmstbpinctrl_pin bcmstbpinctrl_d0_aon_pins[] = {
	/* D0 GPIO mux/pad offsets verified against the Linux binding tables. */
	{ "aon_gpio2", 3, 2, 5, 11, { "gpio" } },
	{ "aon_gpio3", 3, 3, 5, 12, { "gpio" } },
	{ "aon_gpio4", 3, 4, 5, 13, { "gpio" } },
	{ "aon_gpio5", 3, 5, 5, 14, { "gpio", NULL, NULL, NULL, "sd_card_g" } },
};

struct bcm2712_pin_op {
	uint32_t offset, mask, value;
};

/* bias == -1 preserves the pad; an empty function preserves the mux. */
static int
bcm2712_pin_plan(int aon, const char *name, const char *function, int bias,
    struct bcm2712_pin_op *ops, unsigned *count, unsigned capacity)
{
	const struct bcmstbpinctrl_pin *pins, *pin;
	unsigned i, n, f, needed;
	pins = aon ? bcmstbpinctrl_d0_aon_pins : bcmstbpinctrl_d0_pins;
	n = aon ? sizeof(bcmstbpinctrl_d0_aon_pins) / sizeof(*pins) :
		  sizeof(bcmstbpinctrl_d0_pins) / sizeof(*pins);
	for (i = 0; i < n; i++)
		if (strcmp(name, pins[i].name) == 0)
			break;
	if (i == n || bias < -1 || bias > 2)
		return (EINVAL);
	pin = &pins[i];
	f = 0;
	if (*function != '\0') {
		for (; f < 10; f++)
			if (pin->func[f] != NULL &&
			    strcmp(function, pin->func[f]) == 0)
				break;
		if (f == 10)
			return (EOPNOTSUPP);
	}
	needed = (*function != '\0') + (bias >= 0);
	if (*count > capacity || needed > capacity - *count)
		return (E2BIG);
	if (*function != '\0') {
		ops[*count] = (struct bcm2712_pin_op) { pin->func_reg * 4,
			0xfU << (pin->func_off * 4), f << (pin->func_off * 4) };
		(*count)++;
	}
	if (bias >= 0) {
		ops[*count] = (struct bcm2712_pin_op) { pin->bias_reg * 4,
			3U << (pin->bias_off * 2),
			(uint32_t)bias << (pin->bias_off * 2) };
		(*count)++;
	}
	return (0);
}

static void
bcm2712_pin_apply(const struct bcm2712_pin_op *ops, unsigned count,
    uint32_t (*read_reg)(void *, uint32_t),
    void (*write_reg)(void *, uint32_t, uint32_t), void *arg)
{
	unsigned i;
	uint32_t value;
	for (i = 0; i < count; i++) {
		value = read_reg(arg, ops[i].offset);
		value = (value & ~ops[i].mask) | ops[i].value;
		write_reg(arg, ops[i].offset, value);
	}
}
#endif
