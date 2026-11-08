/* $OpenBSD: bcmstbgpio.c,v 1.2 2025/09/08 19:32:18 kettenis Exp $ */
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
 * FreeBSD adaptation for the BCM2712 D0 AON SD bootstrap. Physical GPIO
 * numbers retain their 32-pin bank stride; invalid bank-width holes are not
 * compacted. GPIO2 releases RP1 reset, GPIO3/4 control the SD supplies, and
 * GPIO5 samples card detect. Other pins, pulls and GPIO interrupts are not
 * provided by this bootstrap profile. The pinctrl provider owns pad muxing;
 * active-low interpretation belongs to the common GPIO consumer framework.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <machine/bus.h>
#include <dev/ofw/openfirm.h>
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include "gpio_if.h"

#define BCM2712_AON_GPIO_BASE UINT64_C(0x107d517c00)
#define GIO_DATA(bank) (0x04 + (bank) * 32)
#define GIO_IODIR(bank) (0x08 + (bank) * 32)
#define GIO_MASK(bank) (0x14 + (bank) * 32)
#define BCMSTB_BANKS 2
#define BCMSTB_CAPS (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT)
#define BCMSTB_PRESET (GPIO_PIN_PRESET_LOW | GPIO_PIN_PRESET_HIGH)

struct bcmstb_gpio_hog {
	uint32_t pin, flags;
	gpio_pin_t handle;
};

struct bcmstb_gpio_softc {
	device_t busdev;
	device_t pinctrl;
	struct resource *mem;
	struct mtx lock;
	bool lock_initialized;
	uint32_t widths[BCMSTB_BANKS];
	uint32_t saved_data[BCMSTB_BANKS];
	uint32_t saved_dir[BCMSTB_BANKS];
	uint32_t saved_mask[BCMSTB_BANKS];
	bool saved;
	bool ready, published, hog_write;
	struct bcmstb_gpio_hog hogs[BCMSTB_BANKS * 32];
	unsigned nhogs;
	uint32_t hogmask[BCMSTB_BANKS];
};

static bool
bcmstb_gpio_valid(struct bcmstb_gpio_softc *sc, uint32_t pin)
{
	return (pin / 32 < BCMSTB_BANKS && pin % 32 < sc->widths[pin / 32]);
}

static uint32_t
bcmstb_gpio_caps(struct bcmstb_gpio_softc *sc, uint32_t pin)
{
	if (!bcmstb_gpio_valid(sc, pin))
		return (0);
	if (pin >= 2 && pin <= 4)
		return (BCMSTB_CAPS);
	return (pin == 5 ? GPIO_PIN_INPUT : 0);
}

/* Selected D0 AON profile only: validate the entire child plan before MMIO.
 * The local gpio-hog binding uses two cells per pin, not a provider phandle.
 */
static int
bcmstb_gpio_hog_plan(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	phandle_t child;
	pcell_t cells[BCMSTB_BANKS * 32 * 2];
	char property[64], next[64], text[128];
	const char *previous;
	uint32_t pin, flags;
	int length, i, modes, result;
	bool input, high, low;

	for (child = OF_child(ofw_bus_get_node(dev)); child != 0;
	    child = OF_peer(child)) {
		if (!ofw_bus_node_status_okay(child))
			continue;
		if (!OF_hasprop(child, "gpio-hog") ||
		    OF_getproplen(child, "gpio-hog") != 0 || OF_child(child) != 0)
			return (EOPNOTSUPP);
		/* Reject unsupported properties, rather than silently ignoring policy. */
		previous = NULL;
		while ((result = OF_nextprop(child, previous, next, sizeof(next))) > 0) {
			if (strnlen(next, sizeof(next)) == sizeof(next))
				return (EINVAL);
			if (strcmp(next, "gpio-hog") && strcmp(next, "gpios") &&
			    strcmp(next, "input") && strcmp(next, "output-high") &&
			    strcmp(next, "output-low") && strcmp(next, "line-name") &&
			    strcmp(next, "name") && strcmp(next, "status") &&
			    strcmp(next, "phandle") && strcmp(next, "linux,phandle"))
				return (EOPNOTSUPP);
			strlcpy(property, next, sizeof(property));
			previous = property;
		}
		if (result < 0)
			return (EINVAL);
		input = OF_hasprop(child, "input");
		high = OF_hasprop(child, "output-high");
		low = OF_hasprop(child, "output-low");
		modes = input + high + low;
		if (modes != 1 || (input && OF_getproplen(child, "input") != 0) ||
		    (high && OF_getproplen(child, "output-high") != 0) ||
		    (low && OF_getproplen(child, "output-low") != 0))
			return (EINVAL);
		length = OF_getproplen(child, "line-name");
		if (length != -1 && (length <= 0 || length > (int)sizeof(text) ||
		    OF_getprop(child, "line-name", text, sizeof(text)) != length ||
		    text[length - 1] != '\0' || strnlen(text, length) != (size_t)length - 1))
			return (EINVAL);
		length = OF_getproplen(child, "gpios");
		if (length <= 0 || length > (int)sizeof(cells) ||
		    length % (2 * sizeof(pcell_t)) != 0 ||
		    OF_getencprop(child, "gpios", cells, sizeof(cells)) != length)
			return (EINVAL);
		for (i = 0; i < length / (int)sizeof(pcell_t); i += 2) {
			pin = cells[i];
			if (cells[i + 1] & ~1U)
				return (EOPNOTSUPP);
			flags = input ? GPIO_PIN_INPUT : GPIO_PIN_OUTPUT;
			if (!(bcmstb_gpio_caps(sc, pin) & flags))
				return (EOPNOTSUPP);
			if (sc->hogmask[pin / 32] & (1U << (pin % 32)))
				return (EINVAL);
			if (!input)
				flags |= (high != !!(cells[i + 1] & 1)) ?
				    GPIO_PIN_PRESET_HIGH : GPIO_PIN_PRESET_LOW;
			sc->hogmask[pin / 32] |= 1U << (pin % 32);
			sc->hogs[sc->nhogs++] = (struct bcmstb_gpio_hog) {
			    .pin = pin, .flags = flags };
		}
	}
	return (0);
}

static device_t
bcmstb_gpio_get_bus(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	return (sc->busdev);
}

static int
bcmstb_gpio_pin_max(device_t dev, int *maxpin)
{
	*maxpin = BCMSTB_BANKS * 32 - 1;
	return (0);
}

static int
bcmstb_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	if (pin >= BCMSTB_BANKS * 32)
		return (EINVAL);
	*caps = bcmstb_gpio_caps(sc, pin);
	return (0);
}

static int
bcmstb_gpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	if (pin >= BCMSTB_BANKS * 32)
		return (EINVAL);
	snprintf(name, GPIOMAXNAME, "aon_gpio%u", pin);
	return (0);
}

static int
bcmstb_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	if (!sc->lock_initialized || sc->mem == NULL)
		return (ENXIO);
	if (bcmstb_gpio_caps(sc, pin) == 0)
		return (EOPNOTSUPP);
	mtx_lock(&sc->lock);
	*flags = bus_read_4(sc->mem, GIO_IODIR(pin / 32)) & (1U << (pin % 32)) ?
	    GPIO_PIN_INPUT : GPIO_PIN_OUTPUT;
	mtx_unlock(&sc->lock);
	return (0);
}

static int
bcmstb_gpio_setflags_locked(device_t dev, uint32_t pin, uint32_t flags)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	uint32_t direction = flags & BCMSTB_CAPS;
	uint32_t bank = pin / 32, mask = 1U << (pin % 32), data = 0, value;
	int error;

	if (flags & ~(BCMSTB_CAPS | BCMSTB_PRESET) ||
	    (direction != GPIO_PIN_INPUT && direction != GPIO_PIN_OUTPUT) ||
	    (flags & BCMSTB_PRESET) == BCMSTB_PRESET ||
	    ((flags & BCMSTB_PRESET) && direction != GPIO_PIN_OUTPUT))
		return (EINVAL);
	if (!(bcmstb_gpio_caps(sc, pin) & direction))
		return (EOPNOTSUPP);
	/* Preload the desired output before selecting GPIO and enabling it. */
	if (flags & BCMSTB_PRESET) {
		data = bus_read_4(sc->mem, GIO_DATA(bank));
		value = flags & GPIO_PIN_PRESET_HIGH ? data | mask : data & ~mask;
		bus_write_4(sc->mem, GIO_DATA(bank), value);
		bus_barrier(sc->mem, GIO_DATA(bank), 4, BUS_SPACE_BARRIER_WRITE);
	}
	error = FDT_PINCTRL_SET_FLAGS(sc->pinctrl, dev, pin, flags);
	if (error == 0) {
		value = bus_read_4(sc->mem, GIO_IODIR(bank));
		value = direction == GPIO_PIN_INPUT ? value | mask : value & ~mask;
		bus_write_4(sc->mem, GIO_IODIR(bank), value);
		bus_barrier(sc->mem, GIO_IODIR(bank), 4, BUS_SPACE_BARRIER_WRITE);
	} else if (flags & BCMSTB_PRESET) {
		bus_write_4(sc->mem, GIO_DATA(bank), data);
		bus_barrier(sc->mem, GIO_DATA(bank), 4, BUS_SPACE_BARRIER_WRITE);
	}
	if (error == 0 &&
	    (!!(bus_read_4(sc->mem, GIO_IODIR(bank)) & mask) !=
		(direction == GPIO_PIN_INPUT) ||
		((flags & BCMSTB_PRESET) &&
		 !!(bus_read_4(sc->mem, GIO_DATA(bank)) & mask) !=
		 !!(flags & GPIO_PIN_PRESET_HIGH))))
		error = EIO;
	return (error);
}

static int
bcmstb_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	int error;

	if (!sc->ready)
		return (ENXIO);
	if (!bcmstb_gpio_caps(sc, pin))
		return (EOPNOTSUPP);
	mtx_lock(&sc->lock);
	if (sc->hogmask[pin / 32] & (1U << (pin % 32)))
		error = EBUSY;
	else
		error = bcmstb_gpio_setflags_locked(dev, pin, flags);
	mtx_unlock(&sc->lock);
	return (error);
}

static int
bcmstb_gpio_pin_get(device_t dev, uint32_t pin, uint32_t *value)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	if (!sc->lock_initialized || sc->mem == NULL)
		return (ENXIO);
	if (!bcmstb_gpio_caps(sc, pin))
		return (EOPNOTSUPP);
	mtx_lock(&sc->lock);
	*value = !!(bus_read_4(sc->mem, GIO_DATA(pin / 32)) & (1U << (pin % 32)));
	mtx_unlock(&sc->lock);
	return (0);
}

static int
bcmstb_gpio_write(device_t dev, uint32_t pin, uint32_t value, bool toggle)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	uint32_t data, mask = 1U << (pin % 32);
	if (!sc->ready)
		return (ENXIO);
	if (value > 1)
		return (EINVAL);
	if (!(bcmstb_gpio_caps(sc, pin) & GPIO_PIN_OUTPUT))
		return (EOPNOTSUPP);
	mtx_lock(&sc->lock);
	if (sc->hogmask[pin / 32] & mask) {
		mtx_unlock(&sc->lock);
		return (EBUSY);
	}
	data = bus_read_4(sc->mem, GIO_DATA(pin / 32));
	data = toggle ? data ^ mask : value ? data | mask : data & ~mask;
	bus_write_4(sc->mem, GIO_DATA(pin / 32), data);
	bus_barrier(sc->mem, GIO_DATA(pin / 32), 4, BUS_SPACE_BARRIER_WRITE);
	mtx_unlock(&sc->lock);
	return (0);
}

static int
bcmstb_gpio_pin_set(device_t dev, uint32_t pin, uint32_t value)
{
	return (bcmstb_gpio_write(dev, pin, value, false));
}

static int
bcmstb_gpio_pin_toggle(device_t dev, uint32_t pin)
{
	return (bcmstb_gpio_write(dev, pin, 0, true));
}

static int
bcmstb_gpio_map_gpios(device_t dev, phandle_t consumer, phandle_t parent,
    int ncells, pcell_t *cells, uint32_t *pin, uint32_t *flags)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	if (!sc->ready)
		return (ENXIO);
	if (parent != ofw_bus_get_node(dev) || ncells != 2)
		return (EINVAL);
	if (cells[1] & ~1U) /* Only standard active-low needs no extra pad support. */
		return (EOPNOTSUPP);
	if (!bcmstb_gpio_caps(sc, cells[0]))
		return (EOPNOTSUPP);
	if (sc->hogmask[cells[0] / 32] & (1U << (cells[0] % 32)))
		return (EBUSY);
	*pin = cells[0];
	*flags = cells[1];
	return (0);
}

static int
bcmstb_gpio_probe(device_t dev)
{
	phandle_t node = ofw_bus_get_node(dev), sibling;
	uint32_t reg[2];

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "brcm,brcmstb-gpio"))
		return (ENXIO);
	/* Do not claim other STB GPIO banks or SoCs with this D0-only port. */
	if (OF_getproplen(node, "reg") != sizeof(reg) ||
	    OF_getencprop(node, "reg", reg, sizeof(reg)) != sizeof(reg) ||
	    reg[0] != 0x7d517c00 || reg[1] != BCMSTB_BANKS * 32)
		return (ENXIO);
	for (sibling = OF_child(OF_parent(node)); sibling != 0;
	    sibling = OF_peer(sibling)) {
		if (ofw_bus_node_status_okay(sibling) &&
		    ofw_bus_node_is_compatible(sibling, "brcm,bcm2712d0-aon-pinctrl")) {
			device_set_desc(dev, "BCM2712 D0 AON GPIO SD bootstrap (polled)");
			return (BUS_PROBE_DEFAULT);
		}
	}
	return (ENXIO);
}

/* No complete pad rollback exists. Once a hog may have changed a pad, retain
 * every owner unpublished; pretending an attach error freed it would be unsafe.
 */
static int
bcmstb_gpio_release(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	int bank, error;
	unsigned i;

	if (sc->published || sc->hog_write)
		return (EBUSY);
	for (i = 0; i < sc->nhogs; i++) {
		if (sc->hogs[i].handle != NULL) {
			gpio_pin_release(sc->hogs[i].handle);
			sc->hogs[i].handle = NULL;
		}
	}
	if (sc->busdev != NULL) {
		error = device_delete_child(dev, sc->busdev);
		if (error != 0)
			return (error);
		sc->busdev = NULL;
	}
	if (sc->saved) {
		for (bank = 0; bank < BCMSTB_BANKS; bank++) {
			bus_write_4(sc->mem, GIO_MASK(bank), sc->saved_mask[bank]);
			bus_barrier(sc->mem, GIO_MASK(bank), 4, BUS_SPACE_BARRIER_WRITE);
			if (bus_read_4(sc->mem, GIO_MASK(bank)) != sc->saved_mask[bank])
				return (EIO);
		}
		sc->saved = false;
	}
	if (sc->mem != NULL) {
		error = bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem);
		if (error != 0)
			return (error);
		sc->mem = NULL;
	}
	if (sc->lock_initialized) {
		mtx_destroy(&sc->lock);
		sc->lock_initialized = false;
	}
	return (0);
}

static int
bcmstb_gpio_attach(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	phandle_t node = ofw_bus_get_node(dev), sibling;
	uint32_t cells;
	int bank, error = ENXIO, cleanup, rid = 0;
	unsigned i;

	sc->pinctrl = NULL;
	if (OF_getproplen(node, "brcm,gpio-bank-widths") != sizeof(sc->widths) ||
	    OF_getencprop(node, "brcm,gpio-bank-widths", sc->widths,
	    sizeof(sc->widths)) != sizeof(sc->widths) ||
	    sc->widths[0] != 15 || sc->widths[1] != 6 ||
	    OF_getencprop(node, "#gpio-cells", &cells, sizeof(cells)) != sizeof(cells) ||
	    cells != 2 || !OF_hasprop(node, "gpio-controller"))
		return (EINVAL);
	error = bcmstb_gpio_hog_plan(dev);
	if (error != 0)
		return (error);
	error = ENXIO;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL)
		return (ENXIO);
	if (rman_get_start(sc->mem) != BCM2712_AON_GPIO_BASE ||
	    rman_get_size(sc->mem) != BCMSTB_BANKS * 32) {
		device_printf(dev, "unsupported AON GPIO resource profile\n");
		goto fail;
	}
	for (sibling = OF_child(OF_parent(node)); sibling != 0;
	    sibling = OF_peer(sibling)) {
		if (ofw_bus_node_status_okay(sibling) &&
		    ofw_bus_node_is_compatible(sibling, "brcm,bcm2712d0-aon-pinctrl")) {
			sc->pinctrl = OF_device_from_xref(OF_xref_from_node(sibling));
			break;
		}
	}
	if (sc->pinctrl == NULL) {
		device_printf(dev, "required D0 AON pinctrl provider is unavailable\n");
		goto fail;
	}
	mtx_init(&sc->lock, device_get_nameunit(dev), NULL, MTX_DEF);
	sc->lock_initialized = true;
	for (bank = 0; bank < BCMSTB_BANKS; bank++) {
		sc->saved_data[bank] = bus_read_4(sc->mem, GIO_DATA(bank));
		sc->saved_dir[bank] = bus_read_4(sc->mem, GIO_IODIR(bank));
		sc->saved_mask[bank] = bus_read_4(sc->mem, GIO_MASK(bank));
		bus_write_4(sc->mem, GIO_MASK(bank), 0);
	}
	bus_barrier(sc->mem, 0, BCMSTB_BANKS * 32, BUS_SPACE_BARRIER_WRITE);
	sc->saved = true;
	sc->busdev = gpiobus_add_bus_deferred(dev);
	if (sc->busdev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	bus_attach_children(dev);
	if (!device_is_attached(sc->busdev) || !gpiobus_ready(sc->busdev))
		goto fail;
	/* Reserve the complete plan before the first DATA, mux or direction write. */
	for (i = 0; i < sc->nhogs; i++) {
		error = gpio_pin_get_by_bus_pinnum(sc->busdev, sc->hogs[i].pin,
		    &sc->hogs[i].handle);
		if (error != 0)
			goto fail;
	}
	for (i = 0; i < sc->nhogs; i++) {
		sc->hog_write = true;
		mtx_lock(&sc->lock);
		error = bcmstb_gpio_setflags_locked(dev, sc->hogs[i].pin,
		    sc->hogs[i].flags);
		mtx_unlock(&sc->lock);
		if (error != 0)
			goto fail;
	}
	sc->ready = true;
	ofw_gpiobus_register_provider(dev);
	sc->published = true;
	return (0);
fail:
	cleanup = bcmstb_gpio_release(dev);
	if (cleanup == 0)
		return (error);
	if (sc->busdev != NULL)
		gpiobus_quarantine(sc->busdev, error);
	device_printf(dev, "GPIO unavailable: retaining unpublished hog owners (%d/%d)\n",
	    error, cleanup);
	return (0); /* Retain the softc and all unreleased owners. */
}

static int
bcmstb_gpio_detach(device_t dev)
{
	return (bcmstb_gpio_release(dev));
}

static void
bcmstb_gpio_new_pass(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	if (sc->ready)
		bus_generic_new_pass(dev);
}

static void
bcmstb_gpio_driver_added(device_t dev, driver_t *driver)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	if (sc->ready)
		bus_generic_driver_added(dev, driver);
}

static int
bcmstb_gpio_rescan(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	if (!sc->ready)
		return (ENXIO);
	bus_attach_children(dev);
	return (0);
}

static int
bcmstb_gpio_suspend(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	return (sc->ready ? bus_generic_suspend(dev) : ENXIO);
}

static int
bcmstb_gpio_resume(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	return (sc->ready ? bus_generic_resume(dev) : ENXIO);
}

static int
bcmstb_gpio_shutdown(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	return (sc->ready ? bus_generic_shutdown(dev) : ENXIO);
}

static device_method_t bcmstb_gpio_methods[] = {
	DEVMETHOD(device_probe, bcmstb_gpio_probe),
	DEVMETHOD(device_attach, bcmstb_gpio_attach),
	DEVMETHOD(device_detach, bcmstb_gpio_detach),
	DEVMETHOD(device_suspend, bcmstb_gpio_suspend),
	DEVMETHOD(device_resume, bcmstb_gpio_resume),
	DEVMETHOD(device_shutdown, bcmstb_gpio_shutdown),
	DEVMETHOD(bus_new_pass, bcmstb_gpio_new_pass),
	DEVMETHOD(bus_driver_added, bcmstb_gpio_driver_added),
	DEVMETHOD(bus_rescan, bcmstb_gpio_rescan),
	DEVMETHOD(gpio_get_bus, bcmstb_gpio_get_bus),
	DEVMETHOD(gpio_pin_max, bcmstb_gpio_pin_max),
	DEVMETHOD(gpio_pin_getname, bcmstb_gpio_pin_getname),
	DEVMETHOD(gpio_pin_getcaps, bcmstb_gpio_pin_getcaps),
	DEVMETHOD(gpio_pin_getflags, bcmstb_gpio_pin_getflags),
	DEVMETHOD(gpio_pin_setflags, bcmstb_gpio_pin_setflags),
	DEVMETHOD(gpio_pin_get, bcmstb_gpio_pin_get),
	DEVMETHOD(gpio_pin_set, bcmstb_gpio_pin_set),
	DEVMETHOD(gpio_pin_toggle, bcmstb_gpio_pin_toggle),
	DEVMETHOD(gpio_map_gpios, bcmstb_gpio_map_gpios),
	DEVMETHOD_END
};
static driver_t bcmstb_gpio_driver = {
	"bcmstbgpio", bcmstb_gpio_methods, sizeof(struct bcmstb_gpio_softc)
};
EARLY_DRIVER_MODULE(bcmstbgpio, simplebus, bcmstb_gpio_driver, NULL, NULL,
    BUS_PASS_RESOURCE + BUS_PASS_ORDER_MIDDLE);
MODULE_DEPEND(bcmstbgpio, gpiobus, 1, 1, 1);
MODULE_VERSION(bcmstbgpio, 1);
