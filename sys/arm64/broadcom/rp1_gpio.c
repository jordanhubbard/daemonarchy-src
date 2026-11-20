/* $OpenBSD: rpigpio.c,v 1.2 2025/09/16 08:42:59 kettenis Exp $ */
/*
 * Copyright (c) 2024 Patrick Wildt <patrick@blueri.se>
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

/* FreeBSD polled GPIO adaptation. Alternate pin mux and GPIO IRQs are separate. */
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
#include <machine/resource.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include "gpio_if.h"

#define RP1_GPIO_PINS 54
#define RP1_GPIO_SIZE 0xc000
#define RP1_GPIO_CTRL(n) (4 + 8 * (n))
#define RP1_GPIO_FUNC_MASK 0x1f
#define RP1_GPIO_FUNC 5
#define RP1_GPIO_OUTOVER (3U << 12)
#define RP1_GPIO_OEOVER (3U << 14)
#define RP1_GPIO_OE_DISABLE (2U << 14)
#define RP1_GPIO_INOVER (3U << 16)
#define RP1_GPIO_SET 0x2000
#define RP1_GPIO_CLR 0x3000
#define RP1_GPIO_XOR 0x1000
#define RP1_RIO_OUT 0
#define RP1_RIO_OE 4
#define RP1_RIO_IN 8
#define RP1_PAD(n) (4 + 4 * (n))
#define RP1_PAD_OD (1U << 7)
#define RP1_PAD_IE (1U << 6)
#define RP1_PAD_UP (1U << 3)
#define RP1_PAD_DOWN (1U << 2)
#define RP1_GPIO_CAPS (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT | GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN)
#define RP1_GPIO_PRESET (GPIO_PIN_PRESET_LOW | GPIO_PIN_PRESET_HIGH)

struct rp1_gpio_softc {
	device_t busdev;
	struct resource *mem[3]; /* IO, RIO, PADS */
	struct mtx lock;
	bool lock_initialized, ready, published;
};

/* RP1 banks are 28/6/20 pins, rather than groups of 32. */
static int
rp1_gpio_location(uint32_t pin, unsigned *bank, unsigned *local)
{

	if (pin >= RP1_GPIO_PINS)
		return (EINVAL);
	*bank = pin < 28 ? 0 : pin < 34 ? 1 : 2;
	*local = pin - (*bank == 0 ? 0 : *bank == 1 ? 28 : 34);
	return (0);
}

static uint32_t
rp1_gpio_read(struct rp1_gpio_softc *sc, unsigned region, unsigned bank,
    unsigned offset)
{

	return (bus_read_4(sc->mem[region], bank * 0x4000 + offset));
}

static void
rp1_gpio_write(struct rp1_gpio_softc *sc, unsigned region, unsigned bank,
    unsigned offset, uint32_t value)
{

	bus_write_4(sc->mem[region], bank * 0x4000 + offset, value);
	/* All aliases map the same register; read its RW view to flush PCIe. */
	(void)rp1_gpio_read(sc, region, bank, offset & 0xfff);
}

static device_t
rp1_gpio_bus(device_t dev)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	return (sc->busdev);
}

static phandle_t
rp1_gpio_node(device_t dev, device_t child)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	return (child != NULL && child == sc->busdev ? ofw_bus_get_node(dev) : -1);
}

static int
rp1_gpio_max(device_t dev, int *maxpin)
{

	*maxpin = RP1_GPIO_PINS - 1;
	return (0);
}

static int
rp1_gpio_name(device_t dev, uint32_t pin, char *name)
{

	if (pin >= RP1_GPIO_PINS)
		return (EINVAL);
	snprintf(name, GPIOMAXNAME, "gpio%u", pin);
	return (0);
}

static int
rp1_gpio_caps(device_t dev, uint32_t pin, uint32_t *caps)
{

	if (pin >= RP1_GPIO_PINS)
		return (EINVAL);
	*caps = RP1_GPIO_CAPS;
	return (0);
}

static int
rp1_gpio_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	unsigned bank, local;
	uint32_t ctrl, pad, oe;

	if (rp1_gpio_location(pin, &bank, &local) != 0)
		return (EINVAL);
	mtx_lock(&sc->lock);
	ctrl = rp1_gpio_read(sc, 0, bank, RP1_GPIO_CTRL(local));
	pad = rp1_gpio_read(sc, 2, bank, RP1_PAD(local));
	oe = rp1_gpio_read(sc, 1, bank, RP1_RIO_OE);
	*flags = 0;
	/* An alternate function is not reported as a configured GPIO. */
	if ((ctrl & (RP1_GPIO_FUNC_MASK | RP1_GPIO_OEOVER |
	    RP1_GPIO_OUTOVER | RP1_GPIO_INOVER)) == RP1_GPIO_FUNC) {
		*flags = (oe & (1U << local)) != 0 && (pad & RP1_PAD_OD) == 0 ?
		    GPIO_PIN_OUTPUT : GPIO_PIN_INPUT;
		if ((pad & RP1_PAD_UP) != 0)
			*flags |= GPIO_PIN_PULLUP;
		if ((pad & RP1_PAD_DOWN) != 0)
			*flags |= GPIO_PIN_PULLDOWN;
	}
	mtx_unlock(&sc->lock);
	return (0);
}

static int
rp1_gpio_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	unsigned bank, local;
	uint32_t ctrl, pad, mask, direction, preset, pull;

	if (rp1_gpio_location(pin, &bank, &local) != 0)
		return (EINVAL);
	if ((flags & ~(RP1_GPIO_CAPS | RP1_GPIO_PRESET)) != 0)
		return (EOPNOTSUPP);
	direction = flags & (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT);
	preset = flags & RP1_GPIO_PRESET;
	pull = flags & (GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN);
	if ((direction != GPIO_PIN_INPUT && direction != GPIO_PIN_OUTPUT) ||
	    preset == RP1_GPIO_PRESET ||
	    (preset != 0 && direction != GPIO_PIN_OUTPUT) ||
	    pull == (GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN))
		return (EINVAL);
	mask = 1U << local;
	mtx_lock(&sc->lock);
	ctrl = rp1_gpio_read(sc, 0, bank, RP1_GPIO_CTRL(local));
	pad = rp1_gpio_read(sc, 2, bank, RP1_PAD(local));
	/* Disconnect the output before switching mux or changing its latch. */
	ctrl &= ~(RP1_GPIO_FUNC_MASK | RP1_GPIO_OUTOVER |
	    RP1_GPIO_OEOVER | RP1_GPIO_INOVER);
	ctrl |= RP1_GPIO_FUNC | RP1_GPIO_OE_DISABLE;
	rp1_gpio_write(sc, 0, bank, RP1_GPIO_CTRL(local), ctrl);
	rp1_gpio_write(sc, 1, bank, RP1_GPIO_CLR + RP1_RIO_OE, mask);
	if (preset != 0)
		rp1_gpio_write(sc, 1, bank,
		    (preset == GPIO_PIN_PRESET_HIGH ? RP1_GPIO_SET : RP1_GPIO_CLR) +
		    RP1_RIO_OUT, mask);
	pad &= ~(RP1_PAD_OD | RP1_PAD_UP | RP1_PAD_DOWN);
	pad |= RP1_PAD_IE;
	if ((pull & GPIO_PIN_PULLUP) != 0)
		pad |= RP1_PAD_UP;
	if ((pull & GPIO_PIN_PULLDOWN) != 0)
		pad |= RP1_PAD_DOWN;
	rp1_gpio_write(sc, 2, bank, RP1_PAD(local), pad);
	rp1_gpio_write(sc, 0, bank, RP1_GPIO_CTRL(local), ctrl & ~RP1_GPIO_OEOVER);
	if (direction == GPIO_PIN_OUTPUT)
		rp1_gpio_write(sc, 1, bank, RP1_GPIO_SET + RP1_RIO_OE, mask);
	mtx_unlock(&sc->lock);
	return (0);
}

static int
rp1_gpio_get(device_t dev, uint32_t pin, unsigned *value)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	unsigned bank, local;

	if (rp1_gpio_location(pin, &bank, &local) != 0)
		return (EINVAL);
	mtx_lock(&sc->lock);
	*value = !!(rp1_gpio_read(sc, 1, bank, RP1_RIO_IN) & (1U << local));
	mtx_unlock(&sc->lock);
	return (0);
}

static int
rp1_gpio_output(device_t dev, uint32_t pin, uint32_t value, bool toggle)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	unsigned bank, local, alias;

	if (rp1_gpio_location(pin, &bank, &local) != 0 || value > 1)
		return (EINVAL);
	alias = toggle ? RP1_GPIO_XOR : value ? RP1_GPIO_SET : RP1_GPIO_CLR;
	mtx_lock(&sc->lock);
	rp1_gpio_write(sc, 1, bank, alias + RP1_RIO_OUT, 1U << local);
	mtx_unlock(&sc->lock);
	return (0);
}

static int
rp1_gpio_set(device_t dev, uint32_t pin, uint32_t value)
{

	return (rp1_gpio_output(dev, pin, value, false));
}

static int
rp1_gpio_toggle(device_t dev, uint32_t pin)
{

	return (rp1_gpio_output(dev, pin, 0, true));
}

static int
rp1_gpio_map(device_t dev, phandle_t consumer, phandle_t parent,
    int ncells, pcell_t *cells, uint32_t *pin, uint32_t *flags)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	if (!sc->ready)
		return (ENXIO);
	if (parent != ofw_bus_get_node(dev) || ncells != 2 ||
	    cells[0] >= RP1_GPIO_PINS)
		return (EINVAL);
	if ((cells[1] & ~1U) != 0)
		return (EOPNOTSUPP);
	*pin = cells[0];
	*flags = cells[1]; /* gpiobus applies active-low once, at the consumer API. */
	return (0);
}

static int
rp1_gpio_release(device_t dev)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	int error, i;

	/* Cross-tree GPIO handles have no provider detach reference tracking. */
	if (sc->published)
		return (EBUSY);
	if (sc->busdev != NULL) {
		error = device_delete_child(dev, sc->busdev);
		if (error != 0)
			return (error);
		sc->busdev = NULL;
	}
	for (i = 2; i >= 0; i--) {
		if (sc->mem[i] == NULL)
			continue;
		error = bus_release_resource(dev, SYS_RES_MEMORY, i, sc->mem[i]);
		if (error != 0)
			return (error);
		sc->mem[i] = NULL;
	}
	if (sc->lock_initialized) {
		mtx_destroy(&sc->lock);
		sc->lock_initialized = false;
	}
	return (0);
}

static int
rp1_gpio_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "raspberrypi,rp1-gpio"))
		return (ENXIO);
	device_set_desc(dev, "RP1 GPIO controller (polled)");
	return (BUS_PROBE_DEFAULT);
}

static int
rp1_gpio_attach(device_t dev)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);
	int i, rid, error = ENXIO;
	pcell_t cells;

	if (OF_getproplen(ofw_bus_get_node(dev), "gpio-controller") != 0 ||
	    OF_getencprop(ofw_bus_get_node(dev), "#gpio-cells", &cells,
	    sizeof(cells)) != sizeof(cells) || cells != 2)
		return (EINVAL);
	for (i = 0; i < 3; i++) {
		rid = i;
		sc->mem[i] = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
		if (sc->mem[i] == NULL || rman_get_size(sc->mem[i]) < RP1_GPIO_SIZE)
			goto fail;
	}
	mtx_init(&sc->lock, device_get_nameunit(dev), NULL, MTX_DEF);
	sc->lock_initialized = true;
	sc->busdev = gpiobus_add_bus_deferred(dev);
	if (sc->busdev == NULL) {
		error = ENOMEM;
		goto fail;
	}
	bus_attach_children(dev);
	if (!device_is_attached(sc->busdev) || !gpiobus_ready(sc->busdev))
		goto fail;
	sc->ready = true;
	ofw_gpiobus_register_provider(dev);
	sc->published = true;
	return (0);
fail:
	if (rp1_gpio_release(dev) == 0)
		return (error);
	if (sc->busdev != NULL)
		gpiobus_quarantine(sc->busdev, error);
	device_printf(dev, "attach failed; retained unpublished GPIO resources\n");
	return (0);
}

static void
rp1_gpio_new_pass(device_t dev)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	if (sc->ready)
		bus_generic_new_pass(dev);
}

static void
rp1_gpio_driver_added(device_t dev, driver_t *driver)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	if (sc->ready)
		bus_generic_driver_added(dev, driver);
}

static int
rp1_gpio_rescan(device_t dev)
{
	struct rp1_gpio_softc *sc = device_get_softc(dev);

	if (!sc->ready)
		return (ENXIO);
	bus_attach_children(dev);
	return (0);
}

static device_method_t rp1_gpio_methods[] = {
	DEVMETHOD(device_probe, rp1_gpio_probe),
	DEVMETHOD(device_attach, rp1_gpio_attach),
	DEVMETHOD(device_detach, rp1_gpio_release),
	DEVMETHOD(bus_new_pass, rp1_gpio_new_pass),
	DEVMETHOD(bus_driver_added, rp1_gpio_driver_added),
	DEVMETHOD(bus_rescan, rp1_gpio_rescan),
	DEVMETHOD(gpio_get_bus, rp1_gpio_bus),
	DEVMETHOD(ofw_bus_get_node, rp1_gpio_node),
	DEVMETHOD(gpio_pin_max, rp1_gpio_max),
	DEVMETHOD(gpio_pin_getname, rp1_gpio_name),
	DEVMETHOD(gpio_pin_getcaps, rp1_gpio_caps),
	DEVMETHOD(gpio_pin_getflags, rp1_gpio_getflags),
	DEVMETHOD(gpio_pin_setflags, rp1_gpio_setflags),
	DEVMETHOD(gpio_pin_get, rp1_gpio_get),
	DEVMETHOD(gpio_pin_set, rp1_gpio_set),
	DEVMETHOD(gpio_pin_toggle, rp1_gpio_toggle),
	DEVMETHOD(gpio_map_gpios, rp1_gpio_map),
	DEVMETHOD_END
};
static driver_t rp1_gpio_driver = {
	"rp1_gpio", rp1_gpio_methods, sizeof(struct rp1_gpio_softc)
};
EARLY_DRIVER_MODULE(rp1_gpio, rp1, rp1_gpio_driver, NULL, NULL,
    BUS_PASS_RESOURCE + BUS_PASS_ORDER_MIDDLE);
extern driver_t ofw_gpiobus_driver;
EARLY_DRIVER_MODULE(ofw_gpiobus, rp1_gpio, ofw_gpiobus_driver, NULL, NULL,
    BUS_PASS_BUS);
MODULE_DEPEND(rp1_gpio, gpiobus, 1, 1, 1);
MODULE_VERSION(rp1_gpio, 1);
