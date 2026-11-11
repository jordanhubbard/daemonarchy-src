/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Val Packett <val@packett.cool>
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

#include "opt_acpi.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <machine/bus.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/controller/xhci.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <dev/acpica/acpivar.h>

#include "generic_xhci.h"

/*
 * A 64-bit xHCI capability describes the engine, not the interconnect.  For
 * example, RPi4 UEFI's SCB0._DMA advertises only [0, 0xbfffffff].  Honor the
 * standard resource instead of guessing a board from the generic PNP HID.
 * This bounded implementation accepts one fixed, zero-based identity memory
 * aperture on the nearest ancestor with _DMA. Translated/disjoint ranges need
 * bus translation support and must not silently fall back to unrestricted DMA.
 */
struct generic_xhci_acpi_dma {
	uint64_t lowaddr;
	unsigned ranges;
};

static ACPI_STATUS
generic_xhci_acpi_dma_resource(ACPI_RESOURCE *res, void *arg)
{
	struct generic_xhci_acpi_dma *dma = arg;
	ACPI_RESOURCE_ADDRESS64 address;

	if (res->Type == ACPI_RESOURCE_TYPE_END_TAG)
		return (AE_OK);
	if (res->Type != ACPI_RESOURCE_TYPE_ADDRESS16 &&
	    res->Type != ACPI_RESOURCE_TYPE_ADDRESS32 &&
	    res->Type != ACPI_RESOURCE_TYPE_ADDRESS64)
		return (AE_SUPPORT);
	if (ACPI_FAILURE(AcpiResourceToAddress64(res, &address)))
		return (AE_BAD_DATA);
	if (address.ResourceType != ACPI_MEMORY_RANGE ||
	    address.ProducerConsumer != ACPI_PRODUCER ||
	    address.Decode != ACPI_POS_DECODE ||
	    address.MinAddressFixed != ACPI_ADDRESS_FIXED ||
	    address.MaxAddressFixed != ACPI_ADDRESS_FIXED ||
	    address.Address.Granularity != 0 ||
	    address.Address.Minimum != 0 ||
	    address.Address.TranslationOffset != 0 ||
	    (address.Address.AddressLength & PAGE_MASK) != 0 ||
	    address.ResourceSource.StringLength > 1)
		return (AE_SUPPORT);
	if (address.Address.AddressLength == 0 ||
	    address.Address.Maximum != address.Address.AddressLength - 1)
		return (AE_BAD_DATA);
	if (dma->ranges != 0)
		return (AE_SUPPORT);
	dma->lowaddr = address.Address.Maximum;
	dma->ranges++;
	return (AE_OK);
}

static int
generic_xhci_acpi_dma_limit(ACPI_HANDLE child, bus_addr_t *lowaddr,
    bool *restricted)
{
	struct generic_xhci_acpi_dma dma;
	ACPI_HANDLE parent, object;
	ACPI_STATUS status;
	unsigned depth;

	*lowaddr = BUS_SPACE_MAXADDR;
	*restricted = false;
	/* _DMA belongs to bus containers and describes their child side. */
	for (depth = 0; depth < 64; depth++, child = parent) {
		status = AcpiGetParent(child, &parent);
		if (status == AE_NULL_ENTRY || status == AE_NOT_FOUND)
			return (0);
		if (ACPI_FAILURE(status))
			return (ENXIO);
		status = AcpiGetHandle(parent, "_DMA", &object);
		if (status == AE_NOT_FOUND)
			continue;
		if (ACPI_FAILURE(status))
			return (ENXIO);
		/* ACPI requires a matching _CRS on a bus with _DMA. */
		if (ACPI_FAILURE(AcpiGetHandle(parent, "_CRS", &object)))
			return (EINVAL);
		memset(&dma, 0, sizeof(dma));
		status = AcpiWalkResources(parent, "_DMA",
		    generic_xhci_acpi_dma_resource, &dma);
		if (ACPI_FAILURE(status) || dma.ranges != 1)
			return (EOPNOTSUPP);
		*lowaddr = dma.lowaddr;
		*restricted = true;
		return (0);
	}
	return (E2BIG);
}

static int
generic_xhci_acpi_attach(device_t dev)
{
	struct xhci_softc *sc = device_get_softc(dev);
	bus_addr_t lowaddr;
	bool restricted;
	int error;

	error = generic_xhci_acpi_dma_limit(acpi_get_handle(dev), &lowaddr,
	    &restricted);
	if (error != 0) {
		device_printf(dev, "unsupported ACPI _DMA aperture: %d\n", error);
		return (error);
	}
	if (restricted) {
		error = bus_dma_tag_create(bus_get_dma_tag(dev), 1, 0,
		    lowaddr, BUS_SPACE_MAXADDR, NULL, NULL, BUS_SPACE_MAXSIZE,
		    BUS_SPACE_UNRESTRICTED, BUS_SPACE_MAXSIZE, 0, NULL, NULL,
		    &sc->sc_owned_dma_tag);
		if (error != 0)
			return (error);
		device_printf(dev, "ACPI _DMA identity aperture ends at %#jx\n",
		    (uintmax_t)lowaddr);
	}
	/* generic_xhci_attach's failure unwind also releases the owned tag. */
	return (generic_xhci_attach(dev));
}

static char *xhci_ids[] = {
	"PNP0D10",
	"PNP0D15",
	NULL,
};

static int
generic_xhci_acpi_probe(device_t dev)
{
	if (ACPI_ID_PROBE(device_get_parent(dev), dev, xhci_ids, NULL) >= 0)
		return (ENXIO);

	device_set_desc(dev, XHCI_HC_DEVSTR);

	return (BUS_PROBE_GENERIC);
}

static device_method_t xhci_acpi_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, generic_xhci_acpi_probe),
	DEVMETHOD(device_attach, generic_xhci_acpi_attach),

	DEVMETHOD_END
};

DEFINE_CLASS_1(xhci, xhci_acpi_driver, xhci_acpi_methods,
    sizeof(struct xhci_softc), generic_xhci_driver);

DRIVER_MODULE(xhci, acpi, xhci_acpi_driver, 0, 0);
MODULE_DEPEND(xhci, usb, 1, 1, 1);
