/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015 Semihalf.
 * Copyright (c) 2015 Stormshield.
 * All rights reserved.
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/priv.h>
#include <sys/rman.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_util.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/controller/xhci.h>
#include <dev/usb/controller/xhcireg.h>

#include "generic_xhci.h"

#if __SIZEOF_LONG__ == 8
#define	IS_DMA_32B	0
#elif __SIZEOF_LONG__ == 4
#define	IS_DMA_32B	1
#else
#error unsupported long size
#endif

static int
generic_xhci_quarantine(device_t dev, int error)
{
	struct xhci_softc *sc = device_get_softc(dev);

	if (sc->sc_owned_dma_tag == NULL)
		return (error);
	sc->sc_owned_dma_failed = error;
	if (sc->sc_owned_dma_child != NULL &&
	    !device_is_attached(sc->sc_owned_dma_child))
		device_disable(sc->sc_owned_dma_child);
	device_printf(dev, "USB unavailable: cleanup failed %d; "
	    "retaining controller and DMA owners for detach retry\n", error);
	return (error);
}

/*
 * Newbus frees the softc when attach returns an error. If an owned-tag
 * controller cannot quiesce, retain its driver and all remaining owners for a
 * later detach retry. This is an unavailable controller, not successful USB.
 */
static int
generic_xhci_attach_failed(device_t dev, int original_error)
{
	struct xhci_softc *sc = device_get_softc(dev);
	bool owned = sc->sc_owned_dma_tag != NULL;
	int error;

	error = generic_xhci_detach(dev);
	if (owned && error != 0) {
		sc->sc_owned_dma_failed = original_error;
		device_printf(dev, "USB unavailable: attach failed %d; cleanup %d; "
		    "retaining controller and DMA owners for detach retry\n",
		    original_error, error);
		return (0);
	}
	return (original_error);
}

int
generic_xhci_attach(device_t dev)
{
	struct xhci_softc *sc = device_get_softc(dev);
	int err = 0, rid = 0;

	sc->sc_bus.parent = dev;
	sc->sc_bus.devices = sc->sc_devices;
	sc->sc_bus.devices_max = XHCI_MAX_DEVICES;

	sc->sc_io_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->sc_io_res == NULL) {
		device_printf(dev, "Failed to map memory\n");
		return (generic_xhci_attach_failed(dev, ENXIO));
	}

	sc->sc_io_tag = rman_get_bustag(sc->sc_io_res);
	sc->sc_io_hdl = rman_get_bushandle(sc->sc_io_res);
	sc->sc_io_size = rman_get_size(sc->sc_io_res);

	sc->sc_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
	    RF_SHAREABLE | RF_ACTIVE);
	if (sc->sc_irq_res == NULL) {
		device_printf(dev, "Failed to allocate IRQ\n");
		return (generic_xhci_attach_failed(dev, ENXIO));
	}

	sc->sc_bus.bdev = device_add_child(dev, "usbus", DEVICE_UNIT_ANY);
	if (sc->sc_bus.bdev == NULL) {
		device_printf(dev, "Failed to add USB device\n");
		return (generic_xhci_attach_failed(dev, ENXIO));
	}

	/* USB detach clears sc_bus.bdev, but does not delete the Newbus child
	 * whose ivars still point into this softc. Keep an independent owner.
	 */
	if (sc->sc_owned_dma_tag != NULL)
		sc->sc_owned_dma_child = sc->sc_bus.bdev;
	device_set_ivars(sc->sc_bus.bdev, &sc->sc_bus);

	sprintf(sc->sc_vendor, XHCI_HC_VENDOR);
	device_set_desc(sc->sc_bus.bdev, XHCI_HC_DEVSTR);

	err = bus_setup_intr(dev, sc->sc_irq_res, INTR_TYPE_BIO | INTR_MPSAFE,
	    NULL, (driver_intr_t *)xhci_interrupt, sc, &sc->sc_intr_hdl);
	if (err != 0) {
		device_printf(dev, "Failed to setup error IRQ, %d\n", err);
		sc->sc_intr_hdl = NULL;
		return (generic_xhci_attach_failed(dev, err));
	}

	err = xhci_init(sc, dev,
	    (sc->sc_quirks & XHCI_QUIRK_DMA_32B) == 0 ? IS_DMA_32B : 1);
	if (err != 0) {
		device_printf(dev, "Failed to init XHCI, with error %d\n", err);
		return (generic_xhci_attach_failed(dev, ENXIO));
	}

	if (sc->sc_owned_dma_tag != NULL)
		sc->sc_owned_dma_started = 1;
	err = xhci_start_controller(sc);
	if (err != 0) {
		device_printf(dev, "Failed to start XHCI controller, with error %d\n", err);
		return (generic_xhci_attach_failed(dev, ENXIO));
	}

	err = device_probe_and_attach(sc->sc_bus.bdev);
	if (err != 0) {
		device_printf(dev, "Failed to initialize USB, with error %d\n", err);
		return (generic_xhci_attach_failed(dev, ENXIO));
	}

	return (0);
}

int
generic_xhci_detach(device_t dev)
{
	struct xhci_softc *sc = device_get_softc(dev);
	int err;

	/* during module unload there are lots of children leftover */
	err = bus_generic_detach(dev);
	if (err != 0)
		return (generic_xhci_quarantine(dev, err));

	/* USB children are detached; halt before releasing controller DMA. */
	if (sc->sc_owned_dma_tag != NULL && sc->sc_owned_dma_started) {
		if (xhci_halt_controller(sc) != 0)
			return (generic_xhci_quarantine(dev, EBUSY));
		sc->sc_owned_dma_started = 0;
	}

	if (sc->sc_owned_dma_child != NULL) {
		err = device_delete_child(dev, sc->sc_owned_dma_child);
		if (err != 0)
			return (generic_xhci_quarantine(dev, err));
		sc->sc_owned_dma_child = NULL;
		sc->sc_bus.bdev = NULL;
	}

	if (sc->sc_irq_res != NULL && sc->sc_intr_hdl != NULL) {
		err = bus_teardown_intr(dev, sc->sc_irq_res, sc->sc_intr_hdl);
		if (err != 0) {
			device_printf(dev, "Could not tear down irq, %d\n", err);
			if (sc->sc_owned_dma_tag != NULL)
				return (generic_xhci_quarantine(dev, err));
		}
		sc->sc_intr_hdl = NULL;
	}

	if (sc->sc_irq_res != NULL) {
		err = bus_release_resource(dev, SYS_RES_IRQ,
		    rman_get_rid(sc->sc_irq_res), sc->sc_irq_res);
		if (err != 0 && sc->sc_owned_dma_tag != NULL)
			return (generic_xhci_quarantine(dev, err));
		sc->sc_irq_res = NULL;
	}

	if (sc->sc_io_res != NULL) {
		err = bus_release_resource(dev, SYS_RES_MEMORY,
		    rman_get_rid(sc->sc_io_res), sc->sc_io_res);
		if (err != 0 && sc->sc_owned_dma_tag != NULL)
			return (generic_xhci_quarantine(dev, err));
		sc->sc_io_res = NULL;
	}

	xhci_uninit(sc);

	/* Child USB tags and mappings are gone before their owned parent. */
	if (sc->sc_owned_dma_tag != NULL) {
		err = bus_dma_tag_destroy(sc->sc_owned_dma_tag);
		if (err != 0)
			return (generic_xhci_quarantine(dev, err));
		sc->sc_owned_dma_tag = NULL;
	}
	sc->sc_owned_dma_failed = 0;
	return (0);
}

/*
 * A retained failed attachment must never publish USB in a later pass or
 * revive it through inherited power methods. Only explicit detach may retry
 * cleanup; the required DMA owners remain held until that succeeds.
 */
static void
generic_xhci_new_pass(device_t dev)
{
	struct xhci_softc *sc = device_get_softc(dev);

	if (sc->sc_owned_dma_failed == 0)
		bus_generic_new_pass(dev);
}

static void
generic_xhci_driver_added(device_t dev, driver_t *driver)
{
	struct xhci_softc *sc = device_get_softc(dev);

	if (sc->sc_owned_dma_failed == 0)
		bus_generic_driver_added(dev, driver);
}

static int
generic_xhci_rescan(device_t dev)
{
	struct xhci_softc *sc = device_get_softc(dev);

	if (sc->sc_owned_dma_failed != 0)
		return (ENXIO);
	return (bus_null_rescan(dev));
}

static int
generic_xhci_suspend(device_t dev)
{
	struct xhci_softc *sc = device_get_softc(dev);

	if (sc->sc_owned_dma_failed != 0)
		return (ENXIO);
	return (bus_generic_suspend(dev));
}

static int
generic_xhci_resume(device_t dev)
{
	struct xhci_softc *sc = device_get_softc(dev);

	if (sc->sc_owned_dma_failed != 0)
		return (ENXIO);
	return (bus_generic_resume(dev));
}

static int
generic_xhci_shutdown(device_t dev)
{
	struct xhci_softc *sc = device_get_softc(dev);

	if (sc->sc_owned_dma_failed != 0)
		return (ENXIO);
	return (bus_generic_shutdown(dev));
}

static device_method_t xhci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_attach, generic_xhci_attach),
	DEVMETHOD(device_detach, generic_xhci_detach),
	DEVMETHOD(bus_new_pass, generic_xhci_new_pass),
	DEVMETHOD(bus_driver_added, generic_xhci_driver_added),
	DEVMETHOD(bus_rescan, generic_xhci_rescan),
	DEVMETHOD(device_suspend, generic_xhci_suspend),
	DEVMETHOD(device_resume, generic_xhci_resume),
	DEVMETHOD(device_shutdown, generic_xhci_shutdown),

	DEVMETHOD_END
};

driver_t generic_xhci_driver = {
	"xhci",
	xhci_methods,
	sizeof(struct xhci_softc),
};
