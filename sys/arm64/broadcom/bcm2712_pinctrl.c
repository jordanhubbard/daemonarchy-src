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

/*
 * FreeBSD adaptation of OpenBSD bcmstbpinctrl.c revision 1.3 (2025/09/19).
 * Supports the BCM2712 D0 SD pins, not every BCM2712 pin function.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "bcm2712_pinctrl_policy.h"

#define BCM2712_MAX_OPS 128
struct bcm2712_pinctrl_softc {
	struct resource *mem;
	struct mtx lock;
	int aon;
};

static uint32_t
bcm2712_pin_read(void *arg, uint32_t offset)
{
	struct bcm2712_pinctrl_softc *sc = arg;
	return (bus_read_4(sc->mem, offset));
}
static void
bcm2712_pin_write(void *arg, uint32_t offset, uint32_t value)
{
	struct bcm2712_pinctrl_softc *sc = arg;
	bus_write_4(sc->mem, offset, value);
	bus_barrier(sc->mem, offset, 4, BUS_SPACE_BARRIER_WRITE);
}

/* Build the entire group before any MMIO, including malformed string checks. */
static int
bcm2712_pin_parse(struct bcm2712_pinctrl_softc *sc, phandle_t node,
    struct bcm2712_pin_op *ops, unsigned *count, unsigned depth)
{
	char function[32], property[128], previous[128], *pins, *p, *end;
	const char *previous_property;
	phandle_t child;
	int len, bias, error, biases, next;
	if (depth > 8)
		return (E2BIG);
	if (!OF_hasprop(node, "pins")) {
		child = OF_child(node);
		if (child == 0)
			return (EINVAL);
		for (; child != 0; child = OF_peer(child)) {
			error = bcm2712_pin_parse(sc, child, ops, count,
			    depth + 1);
			if (error != 0)
				return (error);
		}
		return (0);
	}
	previous_property = NULL;
	while ((next = OF_nextprop(node, previous_property, property,
		    sizeof(property))) > 0) {
		if (memchr(property, '\0', sizeof(property)) == NULL)
			return (EINVAL);
		if (strcmp(property, "pins") != 0 &&
		    strcmp(property, "function") != 0 &&
		    strcmp(property, "bias-disable") != 0 &&
		    strcmp(property, "bias-pull-down") != 0 &&
		    strcmp(property, "bias-pull-up") != 0 &&
		    strcmp(property, "phandle") != 0 &&
		    strcmp(property, "linux,phandle") != 0 &&
		    strcmp(property, "name") != 0)
			return (EOPNOTSUPP);
		if (strncmp(property, "bias-", 5) == 0 &&
		    OF_getproplen(node, property) != 0)
			return (EINVAL);
		strlcpy(previous, property, sizeof(previous));
		previous_property = previous;
	}
	if (next < 0)
		return (EINVAL);
	function[0] = '\0';
	len = OF_getproplen(node, "function");
	if (len >= 0) {
		if (len < 2 || len > sizeof(function) ||
		    OF_getprop(node, "function", function, len) != len ||
		    function[len - 1] != '\0' || strlen(function) != len - 1)
			return (EINVAL);
	}
	biases = 0;
	bias = -1;
	if (OF_hasprop(node, "bias-disable")) {
		biases++;
		bias = 0;
	}
	if (OF_hasprop(node, "bias-pull-down")) {
		biases++;
		bias = 1;
	}
	if (OF_hasprop(node, "bias-pull-up")) {
		biases++;
		bias = 2;
	}
	if (biases > 1 || (bias < 0 && function[0] == '\0'))
		return (EINVAL);
	len = OF_getprop_alloc(node, "pins", (void **)&pins);
	if (len <= 0)
		return (EINVAL);
	error = 0;
	end = pins + len;
	for (p = pins; p < end; p += strlen(p) + 1) {
		if (memchr(p, '\0', end - p) == NULL || *p == '\0') {
			error = EINVAL;
			break;
		}
		error = bcm2712_pin_plan(sc->aon, p, function, bias, ops, count,
		    BCM2712_MAX_OPS);
		if (error != 0)
			break;
	}
	OF_prop_free(pins);
	return (error);
}

static int
bcm2712_pinctrl_configure(device_t dev, phandle_t xref)
{
	struct bcm2712_pinctrl_softc *sc = device_get_softc(dev);
	struct bcm2712_pin_op ops[BCM2712_MAX_OPS];
	phandle_t node, parent;
	unsigned count;
	int error;
	node = OF_node_from_xref(xref);
	/* Only permit configurations below this provider. */
	for (parent = node; parent > 0 && parent != ofw_bus_get_node(dev);
	    parent = OF_parent(parent))
		;
	if (node <= 0 || parent != ofw_bus_get_node(dev))
		return (EINVAL);
	count = 0;
	error = bcm2712_pin_parse(sc, node, ops, &count, 0);
	if (error != 0)
		return (error);
	mtx_lock_spin(&sc->lock);
	bcm2712_pin_apply(ops, count, bcm2712_pin_read, bcm2712_pin_write, sc);
	mtx_unlock_spin(&sc->lock);
	return (0);
}

/* The AON GPIO block has no gpio-ranges in the selected firmware DTB.
 * Its driver resolves the sibling provider and calls this standard interface.
 * Preserve the SD CD native mux on pin5; GPIO data still samples that input.
 */
static int
bcm2712_pinctrl_set_flags(device_t dev, device_t gpio, uint32_t pin,
    uint32_t flags)
{
	struct bcm2712_pinctrl_softc *sc = device_get_softc(dev);
	struct bcm2712_pin_op ops[2];
	char name[12];
	unsigned count;
	pcell_t gpio_reg[2], widths[2];
	int bias, error;
	if (!sc->aon || !ofw_bus_is_compatible(gpio, "brcm,brcmstb-gpio") ||
	    OF_parent(ofw_bus_get_node(gpio)) !=
		OF_parent(ofw_bus_get_node(dev)))
		return (EINVAL);
	if (OF_getencprop(ofw_bus_get_node(gpio), "reg", gpio_reg,
		sizeof(gpio_reg)) != sizeof(gpio_reg) ||
	    gpio_reg[0] != 0x7d517c00 || gpio_reg[1] != 0x40 ||
	    OF_getencprop(ofw_bus_get_node(gpio), "brcm,gpio-bank-widths",
		widths, sizeof(widths)) != sizeof(widths) ||
	    widths[0] != 15 || widths[1] != 6)
		return (EINVAL);
	if (pin < 2 || pin > 5)
		return (EOPNOTSUPP);
	if (flags &
	    ~(GPIO_PIN_INPUT | GPIO_PIN_OUTPUT | GPIO_PIN_PULLUP |
		GPIO_PIN_PULLDOWN | GPIO_PIN_PRESET_HIGH | GPIO_PIN_PRESET_LOW))
		return (EOPNOTSUPP);
	if ((flags & (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT)) ==
		(GPIO_PIN_INPUT | GPIO_PIN_OUTPUT) ||
	    (flags & (GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN)) ==
		(GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN))
		return (EINVAL);
	if (pin == 5 && (flags & GPIO_PIN_OUTPUT))
		return (EOPNOTSUPP);
	if ((flags & (GPIO_PIN_PRESET_HIGH | GPIO_PIN_PRESET_LOW)) != 0 &&
	    ((flags & GPIO_PIN_OUTPUT) == 0 ||
		(flags & (GPIO_PIN_PRESET_HIGH | GPIO_PIN_PRESET_LOW)) ==
		    (GPIO_PIN_PRESET_HIGH | GPIO_PIN_PRESET_LOW)))
		return (EINVAL);
	bias = -1;
	if (flags & GPIO_PIN_PULLUP)
		bias = 2;
	else if (flags & GPIO_PIN_PULLDOWN)
		bias = 1;
	snprintf(name, sizeof(name), "aon_gpio%u", pin);
	count = 0;
	error = bcm2712_pin_plan(1, name, pin == 5 ? "" : "gpio", bias, ops,
	    &count, nitems(ops));
	if (error != 0)
		return (error);
	mtx_lock_spin(&sc->lock);
	if (pin == 5 && ((bus_read_4(sc->mem, 0x0c) >> 20) & 0xf) != 4) {
		mtx_unlock_spin(&sc->lock);
		return (EINVAL);
	}
	bcm2712_pin_apply(ops, count, bcm2712_pin_read, bcm2712_pin_write, sc);
	/* A successful callback promises the requested mux/pad reached hardware. */
	for (unsigned i = 0; i < count; i++) {
		if ((bcm2712_pin_read(sc, ops[i].offset) & ops[i].mask) !=
		    ops[i].value) {
			error = EIO;
			break;
		}
	}
	mtx_unlock_spin(&sc->lock);
	return (error);
}

/* Validate all enabled default groups owned by this provider before publishing
 * it or touching registers. The generic tree helper discards callback errors.
 */
static int
bcm2712_pin_default_plan(device_t dev, phandle_t parent,
    struct bcm2712_pin_op *ops, unsigned *count, unsigned depth)
{
	struct bcm2712_pinctrl_softc *sc = device_get_softc(dev);
	phandle_t node, cfg, owner, *configs;
	int nconfigs, i, error;

	if (depth > 64)
		return (E2BIG);
	for (node = OF_child(parent); node != 0; node = OF_peer(node)) {
		if (!ofw_bus_node_status_okay(node))
			continue;
		error = bcm2712_pin_default_plan(dev, node, ops, count,
		    depth + 1);
		if (error != 0)
			return (error);
		if (!OF_hasprop(node, "pinctrl-0"))
			continue;
		nconfigs = OF_getencprop_alloc_multi(node, "pinctrl-0",
		    sizeof(*configs), (void **)&configs);
		if (nconfigs < 0)
			return (EINVAL);
		error = 0;
		for (i = 0; i < nconfigs; i++) {
			cfg = OF_node_from_xref(configs[i]);
			for (owner = cfg;
			    owner > 0 && owner != ofw_bus_get_node(dev);
			    owner = OF_parent(owner))
				;
			if (owner != ofw_bus_get_node(dev))
				continue;
			error = bcm2712_pin_parse(sc, cfg, ops, count, 0);
			if (error != 0)
				break;
		}
		if (nconfigs > 0)
			OF_prop_free(configs);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
bcm2712_pinctrl_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    (!ofw_bus_is_compatible(dev, "brcm,bcm2712d0-pinctrl") &&
		!ofw_bus_is_compatible(dev, "brcm,bcm2712d0-aon-pinctrl")))
		return (ENXIO);
	device_set_desc(dev, "BCM2712 D0 SD pin control");
	return (BUS_PROBE_DEFAULT);
}
static int
bcm2712_pinctrl_attach(device_t dev)
{
	struct bcm2712_pinctrl_softc *sc = device_get_softc(dev);
	struct bcm2712_pin_op ops[BCM2712_MAX_OPS];
	unsigned count;
	int error, rid;
	sc->aon = ofw_bus_is_compatible(dev, "brcm,bcm2712d0-aon-pinctrl");
	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL)
		return (ENXIO);
	if (rman_get_size(sc->mem) < (sc->aon ? 0x1c : 0x20)) {
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->mem);
		return (ENXIO);
	}
	count = 0;
	error = bcm2712_pin_default_plan(dev, OF_peer(0), ops, &count, 0);
	if (error != 0) {
		device_printf(dev,
		    "unsupported default pin configuration: %d\n", error);
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->mem);
		return (error);
	}
	mtx_init(&sc->lock, "bcm2712 pinctrl", NULL, MTX_SPIN);
	mtx_lock_spin(&sc->lock);
	bcm2712_pin_apply(ops, count, bcm2712_pin_read, bcm2712_pin_write, sc);
	mtx_unlock_spin(&sc->lock);
	fdt_pinctrl_register(dev, NULL);
	return (0);
}
static int
bcm2712_pinctrl_detach(device_t dev)
{
	/* FDT pinctrl has no provider unregister operation; references remain
	 * live. */
	return (EBUSY);
}
static device_method_t bcm2712_pinctrl_methods[] = { DEVMETHOD(device_probe,
							 bcm2712_pinctrl_probe),
	DEVMETHOD(device_attach, bcm2712_pinctrl_attach),
	DEVMETHOD(device_detach, bcm2712_pinctrl_detach),
	DEVMETHOD(fdt_pinctrl_configure, bcm2712_pinctrl_configure),
	DEVMETHOD(fdt_pinctrl_set_flags, bcm2712_pinctrl_set_flags),
	DEVMETHOD_END };
static driver_t bcm2712_pinctrl_driver = { "bcm2712_pinctrl",
	bcm2712_pinctrl_methods, sizeof(struct bcm2712_pinctrl_softc) };
EARLY_DRIVER_MODULE(bcm2712_pinctrl, simplebus, bcm2712_pinctrl_driver, 0, 0,
    BUS_PASS_BUS + BUS_PASS_ORDER_EARLY);
MODULE_VERSION(bcm2712_pinctrl, 1);
