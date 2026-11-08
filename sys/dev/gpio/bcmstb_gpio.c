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
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/openfirm.h>
#include "gpio_if.h"

#define BCM2712_AON_GPIO_BASE UINT64_C(0x107d517c00)
#define GIO_DATA(bank) (0x04 + (bank) * 32)
#define GIO_IODIR(bank) (0x08 + (bank) * 32)
#define GIO_MASK(bank) (0x14 + (bank) * 32)
#define BCMSTB_BANKS 2
#define BCMSTB_CAPS (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT)
#define BCMSTB_PRESET (GPIO_PIN_PRESET_LOW | GPIO_PIN_PRESET_HIGH)

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
	if (bcmstb_gpio_caps(sc, pin) == 0)
		return (EOPNOTSUPP);
	mtx_lock(&sc->lock);
	*flags = bus_read_4(sc->mem, GIO_IODIR(pin / 32)) & (1U << (pin % 32)) ?
	    GPIO_PIN_INPUT : GPIO_PIN_OUTPUT;
	mtx_unlock(&sc->lock);
	return (0);
}

static int
bcmstb_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
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
	mtx_lock(&sc->lock);
	/* Preload the desired output before selecting GPIO and enabling it. */
	if (flags & BCMSTB_PRESET) {
		data = bus_read_4(sc->mem, GIO_DATA(bank));
		value = flags & GPIO_PIN_PRESET_HIGH ? data | mask : data & ~mask;
		bus_write_4(sc->mem, GIO_DATA(bank), value);
	}
	error = FDT_PINCTRL_SET_FLAGS(sc->pinctrl, dev, pin, flags);
	if (error == 0) {
		value = bus_read_4(sc->mem, GIO_IODIR(bank));
		value = direction == GPIO_PIN_INPUT ? value | mask : value & ~mask;
		bus_write_4(sc->mem, GIO_IODIR(bank), value);
	} else if (flags & BCMSTB_PRESET) {
		bus_write_4(sc->mem, GIO_DATA(bank), data);
	}
	mtx_unlock(&sc->lock);
	return (error);
}

static int
bcmstb_gpio_pin_get(device_t dev, uint32_t pin, uint32_t *value)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
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
	if (value > 1)
		return (EINVAL);
	if (!(bcmstb_gpio_caps(sc, pin) & GPIO_PIN_OUTPUT))
		return (EOPNOTSUPP);
	mtx_lock(&sc->lock);
	data = bus_read_4(sc->mem, GIO_DATA(pin / 32));
	data = toggle ? data ^ mask : value ? data | mask : data & ~mask;
	bus_write_4(sc->mem, GIO_DATA(pin / 32), data);
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
	if (parent != ofw_bus_get_node(dev) || ncells != 2)
		return (EINVAL);
	if (cells[1] & ~1U) /* Only standard active-low needs no extra pad support. */
		return (EOPNOTSUPP);
	if (!bcmstb_gpio_caps(sc, cells[0]))
		return (EOPNOTSUPP);
	*pin = cells[0];
	*flags = cells[1];
	return (0);
}

static int
bcmstb_gpio_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "brcm,brcmstb-gpio"))
		return (ENXIO);
	device_set_desc(dev, "BCM2712 D0 AON GPIO SD bootstrap (polled)");
	return (BUS_PROBE_DEFAULT);
}

static void
bcmstb_gpio_release(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	int bank;

	if (sc->busdev != NULL) {
		gpiobus_detach_bus(dev);
		device_delete_children(dev);
		sc->busdev = NULL;
	}
	if (sc->saved) {
		for (bank = 0; bank < BCMSTB_BANKS; bank++) {
			bus_write_4(sc->mem, GIO_DATA(bank), sc->saved_data[bank]);
			bus_write_4(sc->mem, GIO_IODIR(bank), sc->saved_dir[bank]);
			bus_write_4(sc->mem, GIO_MASK(bank), sc->saved_mask[bank]);
		}
		sc->saved = false;
	}
	if (sc->mem != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem);
		sc->mem = NULL;
	}
	if (sc->lock_initialized) {
		mtx_destroy(&sc->lock);
		sc->lock_initialized = false;
	}
}

static int
bcmstb_gpio_attach(device_t dev)
{
	struct bcmstb_gpio_softc *sc = device_get_softc(dev);
	phandle_t node = ofw_bus_get_node(dev), sibling;
	uint32_t cells;
	int bank, error = ENXIO, rid = 0;

	sc->pinctrl = NULL;
	if (OF_getproplen(node, "brcm,gpio-bank-widths") != sizeof(sc->widths) ||
	    OF_getencprop(node, "brcm,gpio-bank-widths", sc->widths,
	    sizeof(sc->widths)) != sizeof(sc->widths) ||
	    sc->widths[0] != 15 || sc->widths[1] != 6 ||
	    OF_getencprop(node, "#gpio-cells", &cells, sizeof(cells)) != sizeof(cells) ||
	    cells != 2 || !OF_hasprop(node, "gpio-controller"))
		return (EINVAL);
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
	sc->saved = true;
	sc->busdev = gpiobus_add_bus(dev);
	if (sc->busdev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	bus_attach_children(dev);
	if (!device_is_attached(sc->busdev))
		goto fail;
	return (0);
fail:
	bcmstb_gpio_release(dev);
	return (error);
}

static int
bcmstb_gpio_detach(device_t dev)
{
	/* External regulator/GPIO pin references are not child-device refs. */
	return (EBUSY);
}

static device_method_t bcmstb_gpio_methods[] = {
	DEVMETHOD(device_probe, bcmstb_gpio_probe),
	DEVMETHOD(device_attach, bcmstb_gpio_attach),
	DEVMETHOD(device_detach, bcmstb_gpio_detach),
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
