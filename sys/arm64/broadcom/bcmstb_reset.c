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
 * FreeBSD adaptation of OpenBSD bcmstbreset.c 1.1 and bcmstbrescal.c 1.1.
 * SW_INIT uses SET/CLEAR/STATUS banks. RESCAL is a self-deasserting pulse:
 * the DT-compatible deassert operation executes calibration; assert verifies
 * idle only, and does not claim that a level reset has been held or calibrated.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <machine/bus.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/hwreset/hwreset.h>
#include "hwreset_if.h"

#define SW_INIT_STRIDE 0x18
#define SW_INIT_SET 0x00
#define SW_INIT_CLEAR 0x04
#define SW_INIT_STATUS 0x08
#define RESCAL_START 0x00
#define RESCAL_STATUS 0x08
#define RESCAL_BIT 1U
#define RESET_POLL_US 100
#define RESET_TIMEOUT_US 1000

struct bcmstb_reset_softc {
	struct resource *mem;
	struct mtx lock;
	unsigned nbanks;
	bool rescal;
	bool published;
	bool lock_initialized;
};

static int
bcmstb_reset_valid(struct bcmstb_reset_softc *sc, intptr_t id)
{
	if (!sc->published)
		return (ENXIO);
	if (id < 0 || (sc->rescal ? id != 0 : (uintmax_t)id >= sc->nbanks * 32U))
		return (EINVAL);
	return (0);
}

static int
bcmstb_reset_wait(struct bcmstb_reset_softc *sc, uint32_t offset,
    uint32_t mask, bool asserted)
{
	unsigned elapsed;

	for (elapsed = 0; ; elapsed += RESET_POLL_US) {
		if (((bus_read_4(sc->mem, offset) & mask) != 0) == asserted)
			return (0);
		if (elapsed == RESET_TIMEOUT_US)
			return (ETIMEDOUT);
		DELAY(RESET_POLL_US);
	}
}

static int
bcmstb_reset_assert(device_t dev, intptr_t id, bool asserted)
{
	struct bcmstb_reset_softc *sc = device_get_softc(dev);
	uint32_t bank, bit, start;
	int error;

	error = bcmstb_reset_valid(sc, id);
	if (error != 0)
		return (error);
	mtx_lock(&sc->lock);
	if (!sc->rescal) {
		bank = id / 32;
		bit = 1U << (id % 32);
		bus_write_4(sc->mem, bank * SW_INIT_STRIDE +
		    (asserted ? SW_INIT_SET : SW_INIT_CLEAR), bit);
		bus_barrier(sc->mem, bank * SW_INIT_STRIDE,
		    SW_INIT_STRIDE, BUS_SPACE_BARRIER_WRITE);
		error = bcmstb_reset_wait(sc, bank * SW_INIT_STRIDE +
		    SW_INIT_STATUS, bit, asserted);
		/* Allow the downstream block to settle after reset release. */
		if (error == 0 && !asserted)
			DELAY(100);
	} else {
		start = bus_read_4(sc->mem, RESCAL_START);
		if ((start & RESCAL_BIT) != 0) {
			error = EBUSY;
			goto out;
		}
		if (asserted) {
			/* A pulse controller cannot hold an asserted level. */
			error = 0;
			goto out;
		}
		bus_write_4(sc->mem, RESCAL_START, start | RESCAL_BIT);
		bus_barrier(sc->mem, RESCAL_START, 4, BUS_SPACE_BARRIER_WRITE);
		if ((bus_read_4(sc->mem, RESCAL_START) & RESCAL_BIT) == 0) {
			error = EIO;
			goto out;
		}
		error = bcmstb_reset_wait(sc, RESCAL_STATUS, RESCAL_BIT, true);
		if (error != 0)
			goto out; /* Preserve a timed-out/in-flight request. */
		start = bus_read_4(sc->mem, RESCAL_START);
		bus_write_4(sc->mem, RESCAL_START, start & ~RESCAL_BIT);
		bus_barrier(sc->mem, RESCAL_START, 4, BUS_SPACE_BARRIER_WRITE);
		if ((bus_read_4(sc->mem, RESCAL_START) & RESCAL_BIT) != 0)
			error = EIO;
	}
out:
	mtx_unlock(&sc->lock);
	return (error);
}

static int
bcmstb_reset_is_asserted(device_t dev, intptr_t id, bool *asserted)
{
	struct bcmstb_reset_softc *sc = device_get_softc(dev);
	int error;

	error = bcmstb_reset_valid(sc, id);
	if (error != 0)
		return (error);
	if (asserted == NULL)
		return (EINVAL);
	if (sc->rescal)
		return (EOPNOTSUPP); /* No persistent reset-line state exists. */
	mtx_lock(&sc->lock);
	*asserted = (bus_read_4(sc->mem, (id / 32) * SW_INIT_STRIDE +
	    SW_INIT_STATUS) & (1U << (id % 32))) != 0;
	mtx_unlock(&sc->lock);
	return (0);
}

static int
bcmstb_reset_map(device_t dev, phandle_t xref, int ncells, pcell_t *cells,
    intptr_t *id)
{
	struct bcmstb_reset_softc *sc = device_get_softc(dev);
	intptr_t mapped;
	int error;

	if (id == NULL || xref != OF_xref_from_node(ofw_bus_get_node(dev)) ||
	    (sc->rescal ? ncells != 0 : ncells != 1 || cells == NULL))
		return (EINVAL);
	mapped = sc->rescal ? 0 : cells[0];
	error = bcmstb_reset_valid(sc, mapped);
	if (error == 0)
		*id = mapped;
	return (error);
}

static int
bcmstb_reset_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev) ||
	    (!ofw_bus_is_compatible(dev, "brcm,brcmstb-reset") &&
	    !ofw_bus_is_compatible(dev, "brcm,bcm7216-pcie-sata-rescal")))
		return (ENXIO);
	device_set_desc(dev, "Broadcom STB reset/calibration controller");
	return (BUS_PROBE_DEFAULT);
}

/* Only unpublished providers can release resources. Published hwreset handles
 * are raw provider references, not Newbus child/busy references.
 */
static int
bcmstb_reset_detach(device_t dev)
{
	struct bcmstb_reset_softc *sc = device_get_softc(dev);
	int error;

	if (sc->published)
		return (EBUSY);
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
bcmstb_reset_attach(device_t dev)
{
	struct bcmstb_reset_softc *sc = device_get_softc(dev);
	phandle_t node = ofw_bus_get_node(dev);
	pcell_t cells;
	rman_res_t size;
	int error, rid;

	sc->rescal = ofw_bus_is_compatible(dev, "brcm,bcm7216-pcie-sata-rescal");
	if (node <= 0 || OF_getproplen(node, "#reset-cells") != sizeof(cells) ||
	    OF_getencprop(node, "#reset-cells", &cells,
	    sizeof(cells)) != sizeof(cells) || cells != (sc->rescal ? 0 : 1))
		return (EINVAL);
	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL)
		return (ENXIO);
	size = rman_get_size(sc->mem);
	if (sc->rescal ? size < 0x0c : size < SW_INIT_STRIDE ||
	    size % SW_INIT_STRIDE != 0 || size / SW_INIT_STRIDE > INT_MAX / 32) {
		error = bcmstb_reset_detach(dev);
		if (error != 0) {
			/* Newbus would free the softc on an attach error. Retain
			 * this unpublished, unavailable resource for detach retry.
			 */
			device_printf(dev, "reset provider unavailable: invalid resource; "
			    "cleanup failed %d, retaining owner\n", error);
			return (0);
		}
		return (EINVAL);
	}
	sc->nbanks = sc->rescal ? 0 : size / SW_INIT_STRIDE;
	mtx_init(&sc->lock, "bcmstb reset", NULL, MTX_DEF);
	sc->lock_initialized = true;
	/* Publication is last; attaching a provider does not change reset state. */
	sc->published = true;
	hwreset_register_ofw_provider(dev);
	return (0);
}

static device_method_t bcmstb_reset_methods[] = {
	DEVMETHOD(device_probe, bcmstb_reset_probe),
	DEVMETHOD(device_attach, bcmstb_reset_attach),
	DEVMETHOD(device_detach, bcmstb_reset_detach),
	DEVMETHOD(hwreset_map, bcmstb_reset_map),
	DEVMETHOD(hwreset_assert, bcmstb_reset_assert),
	DEVMETHOD(hwreset_is_asserted, bcmstb_reset_is_asserted),
	DEVMETHOD_END
};
static driver_t bcmstb_reset_driver = {
	"bcmstb_reset", bcmstb_reset_methods, sizeof(struct bcmstb_reset_softc)
};
EARLY_DRIVER_MODULE(bcmstb_reset, simplebus, bcmstb_reset_driver, 0, 0,
    BUS_PASS_RESOURCE + BUS_PASS_ORDER_EARLY);
MODULE_VERSION(bcmstb_reset, 1);
