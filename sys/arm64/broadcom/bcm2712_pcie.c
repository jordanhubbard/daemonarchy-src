/*
 * Copyright (c) 2020, 2025 Mark Kettenis <kettenis@openbsd.org>
 * Copyright (c) 2026 Jordan Hubbard
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

/*-
 * Copyright (c) 2015 Ruslan Bukin <br@bsdpad.com>
 * Copyright (c) 2014,2016 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Andrew Turner under
 * the sponsorship of the FreeBSD Foundation.
 *
 * This software was developed by Semihalf under
 * the sponsorship of the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* Generic ECAM PCIe driver FDT attachment */

/* Selected profile wrapper. Generic allocator/method field contracts are
 * documented in docs/pi5-pcie2-frontend.md. No generic setup/free helper is
 * used: those helpers ignore range/cleanup errors needed by this owner. */
#include "opt_platform.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/queue.h>
#include <sys/intr.h>
#include <machine/bus.h>
#include <machine/intr.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_pci.h>
#include <dev/hwreset/hwreset.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcib_private.h>
#include <dev/pci/pci_host_generic.h>
#include <dev/pci/pci_host_generic_fdt.h>
#include <arm64/broadcom/bcm2712_pcie_hw.h>
#include "pcib_if.h"

struct bcm2712_pcie_softc {
	/* Both inherited driver classes use this exact base-first contract. */
	struct generic_pcie_fdt_softc generic;
	struct bcm2712_pcie_plan plan;
	struct bcm2712_pcie_state state;
	struct resource *mem;
	struct mtx config_lock;
	hwreset_t bridge, rescal;
	bus_dma_tag_t physical;
	device_t dev, msi, gpio, child;
	phandle_t node, gpio_node, msi_node, rp1_node;
	bool lock_initialized, iinfo_initialized, ready, retained;
	bool rman_initialized[3], range_defined[2];
};

static int
cells(phandle_t node, const char *name, uint32_t *out, unsigned count)
{
	int size;
	if (count > INT_MAX / sizeof(*out))
		return (EINVAL);
	size = count * sizeof(*out);
	return (OF_getproplen(node, name) == size &&
	    OF_getencprop(node, name, out, size) == size ? 0 : EINVAL);
}

static int
string_is(phandle_t node, const char *name, const char *expected, size_t size)
{
	char text[128];
	if (size > sizeof(text) || OF_getproplen(node, name) != (int)size ||
	    OF_getprop(node, name, text, size) != (int)size ||
	    memcmp(text, expected, size) != 0)
		return (EINVAL);
	return (0);
}

static int
property_plan(phandle_t node)
{
	static const char * const allowed[] = {"compatible","reg","device_type",
	    "linux,pci-domain","max-link-speed","num-lanes","#address-cells",
	    "#interrupt-cells","#size-cells","interrupt-parent","interrupts",
	    "interrupt-names","interrupt-map-mask","interrupt-map","resets",
	    "reset-names","msi-controller","msi-parent","ranges","dma-ranges",
	    "status","brcm,vdm-qos-map","aspm-no-l0s","phandle","name"};
	char previous[128] = "", next[128];
	unsigned count, i;
	int found;

	for (count = 0; count < 64; count++) {
		memset(next, 0, sizeof(next));
		found = OF_nextprop(node, count == 0 ? NULL : previous, next, sizeof(next));
		if (found == 0)
			return (0);
		if (found < 0 || memchr(next, 0, sizeof(next)) == NULL ||
		    strcmp(previous, next) == 0)
			return (EINVAL);
		for (i = 0; i < nitems(allowed); i++)
			if (strcmp(next, allowed[i]) == 0)
				break;
		if (i == nitems(allowed))
			return (EOPNOTSUPP);
		memcpy(previous, next, sizeof(previous));
	}
	return (E2BIG);
}

static bool
provider_is(device_t dev, phandle_t node, const char *driver)
{
	const char *name;
	if (dev == NULL || !device_is_attached(dev) || ofw_bus_get_node(dev) != node)
		return (false);
	name = device_get_name(dev);
	return (name != NULL && strcmp(name, driver) == 0);
}

static int
provider_plan(struct bcm2712_pcie_softc *sc)
{
	phandle_t bridge, rescal, soc, n, hog, rp1;
	device_t dev;
	uint32_t reg[8], widths[2], value, pin[2];
	unsigned matches = 0, hogs = 0, rp1s = 0, visits;

	bridge = OF_node_from_xref(0x24);
	rescal = OF_node_from_xref(0x23);
	sc->msi_node = OF_node_from_xref(0x27);
	if (bridge <= 0 || rescal <= 0 || sc->msi_node <= 0 ||
	    OF_parent(bridge) != OF_parent(rescal))
		return (EINVAL);
	if (!ofw_bus_node_is_compatible(bridge, "brcm,brcmstb-reset") ||
	    !ofw_bus_node_is_compatible(rescal, "brcm,bcm7216-pcie-sata-rescal") ||
	    !ofw_bus_node_status_okay(bridge) || !ofw_bus_node_status_okay(rescal) ||
	    cells(bridge, "#reset-cells", &value, 1) != 0 || value != 1 ||
	    cells(bridge, "reg", reg, 2) != 0 || reg[0] != 0x1504318 || reg[1] != 0x30 ||
	    cells(rescal, "#reset-cells", &value, 1) != 0 || value != 0 ||
	    cells(rescal, "reg", reg, 2) != 0 || reg[0] != 0x119500 || reg[1] != 0x10)
		return (EINVAL);
	if (!provider_is(OF_device_from_xref(0x24), bridge, "bcmstb_reset") ||
	    !provider_is(OF_device_from_xref(0x23), rescal, "bcmstb_reset"))
		return (ENXIO);
	soc = OF_parent(bridge);
	if (cells(soc, "#address-cells", &value, 1) != 0 || value != 1 ||
	    cells(soc, "#size-cells", &value, 1) != 0 || value != 1)
		return (EINVAL);
	visits = 0;
	for (n = OF_child(soc); n != 0; n = OF_peer(n)) {
		if (n < 0 || ++visits > 256)
			return (EINVAL);
		if (!ofw_bus_node_is_compatible(n, "brcm,brcmstb-gpio") ||
		    cells(n, "reg", reg, 2) != 0 || reg[0] != 0x7d517c00 || reg[1] != 0x40)
			continue;
		matches++;
		sc->gpio_node = n;
	}
	if (matches != 1 || !ofw_bus_node_status_okay(sc->gpio_node) ||
	    cells(sc->gpio_node, "brcm,gpio-bank-widths", widths, 2) != 0 ||
	    widths[0] != 15 || widths[1] != 6 ||
	    cells(sc->gpio_node, "#gpio-cells", &value, 1) != 0 || value != 2)
		return (EINVAL);
	visits = 0;
	for (hog = OF_child(sc->gpio_node); hog != 0; hog = OF_peer(hog)) {
		if (hog < 0 || ++visits > 256)
			return (EINVAL);
		if (string_is(hog, "line-name", "RP1 RUN pin", sizeof("RP1 RUN pin")) != 0)
			continue;
		hogs++;
		if (!ofw_bus_node_status_okay(hog) || OF_getproplen(hog,"gpio-hog") != 0 ||
		    OF_getproplen(hog,"output-high") != 0 ||
		    OF_hasprop(hog,"output-low") || OF_hasprop(hog,"input") ||
		    cells(hog,"gpios",pin,2) != 0 || pin[0] != 2 || pin[1] != 0)
			return (EINVAL);
	}
	if (hogs != 1)
		return (EINVAL);
	/* Published GPIO identity is the driver's post-hog readiness invariant.
	 * Never acquire, change or steal the reserved RUN pin. */
	sc->gpio = OF_device_from_xref(OF_xref_from_node(sc->gpio_node));
	if (!provider_is(sc->gpio, sc->gpio_node, "bcmstbgpio"))
		return (ENXIO);
	if (!ofw_bus_node_is_compatible(sc->msi_node,"brcm,bcm2712-mip") ||
	    !ofw_bus_node_status_okay(sc->msi_node) ||
	    cells(sc->msi_node,"reg",reg,8) != 0 ||
	    reg[0] != 0x10 || reg[1] != 0x130000 || reg[2] != 0 || reg[3] != 0xc0 ||
	    reg[4] != 0xff || reg[5] != 0xfffff000 || reg[6] != 0 || reg[7] != 0x1000)
		return (EINVAL);
	dev = OF_device_from_xref(0x27);
	if (!provider_is(dev, sc->msi_node,"bcm2712_mip"))
		return (ENXIO);
	sc->msi = dev;
	/* RP1 is not a standard PCI OFW reg child. Validate but do not attach
	 * its simple-bus container using an incorrect root DMA domain. */
	visits = 0;
	for (rp1 = OF_child(sc->node); rp1 != 0; rp1 = OF_peer(rp1)) {
		if (rp1 < 0 || ++visits > 256)
			return (EINVAL);
		if (!ofw_bus_node_is_compatible(rp1,"simple-bus"))
			return (EOPNOTSUPP);
		if (OF_hasprop(rp1,"reg"))
			return (EOPNOTSUPP);
		rp1s++;
		sc->rp1_node = rp1;
	}
	if (rp1s != 1)
		return (EINVAL);
	return (intr_msi_validate_provider(sc->msi,0x27));
}

static int
preflight(struct bcm2712_pcie_softc *sc)
{
	uint32_t reg[4], ranges[14], dma[21], resets[3], interrupts[6], imap[32];
	struct bcm2712_pcie_dt d = {0};
	static const char reset_names[] = "rescal\0bridge";
	static const char interrupt_names[] = "pcie\0msi";
	int error;

	error = property_plan(sc->node);
	if (error != 0)
		return (error);
	if (string_is(sc->node,"compatible","brcm,bcm2712-pcie",sizeof("brcm,bcm2712-pcie")) != 0 ||
	    string_is(sc->node,"device_type","pci",sizeof("pci")) != 0 ||
	    string_is(sc->node,"status","okay",sizeof("okay")) != 0 ||
	    string_is(sc->node,"reset-names",reset_names,sizeof(reset_names)) != 0 ||
	    string_is(sc->node,"interrupt-names",interrupt_names,sizeof(interrupt_names)) != 0 ||
	    OF_getproplen(sc->node,"msi-controller") != 0 ||
	    OF_getproplen(sc->node,"aspm-no-l0s") != 0)
		return (EINVAL);
#define CELLS(name, array) do { error=cells(sc->node,name,array,nitems(array)); if(error) return error; } while(0)
	CELLS("reg",reg); CELLS("ranges",ranges); CELLS("dma-ranges",dma);
	CELLS("resets",resets); CELLS("interrupts",interrupts); CELLS("interrupt-map",imap);
	CELLS("interrupt-map-mask",d.interrupt_mask);
#undef CELLS
#define WORD(name, field) do { error=cells(sc->node,name,&d.field,1); if(error) return error; } while(0)
	WORD("#address-cells",acells); WORD("#size-cells",scells); WORD("#interrupt-cells",icells);
	WORD("linux,pci-domain",domain); WORD("num-lanes",lanes); WORD("max-link-speed",generation);
	WORD("msi-parent",msi_parent); WORD("interrupt-parent",interrupt_parent);
#undef WORD
	if (cells(OF_parent(sc->node),"#address-cells",&d.parent_acells,1) != 0 ||
	    cells(OF_parent(sc->node),"#size-cells",&d.parent_scells,1) != 0 ||
	    OF_getproplen(sc->node,"brcm,vdm-qos-map") != sizeof(d.qos) ||
	    OF_getprop(sc->node,"brcm,vdm-qos-map",d.qos,sizeof(d.qos)) != sizeof(d.qos))
		return (EINVAL);
	d.reg=reg;d.nreg=nitems(reg);d.ranges=ranges;d.nranges=nitems(ranges);
	d.dma=dma;d.ndma=nitems(dma);d.resets=resets;d.nresets=nitems(resets);
	d.interrupts=interrupts;d.ninterrupts=nitems(interrupts);d.interrupt_map=imap;d.ninterrupt_map=nitems(imap);
	error = bcm2712_pcie_plan_make(&d,&sc->plan);
	return (error != 0 ? error : provider_plan(sc));
}

static struct rman *
manager(struct bcm2712_pcie_softc *sc, unsigned which)
{
	switch (which) {
	case 0: return (&sc->generic.base.pmem_rman);
	case 1: return (&sc->generic.base.mem_rman);
	default: return (&sc->generic.base.io_rman);
	}
}

/* Called only before any hardware operation/child publication. A busy owner
 * stops cleanup before its backing resources/mutex/reset handles are freed. */
static int
software_cleanup(struct bcm2712_pcie_softc *sc)
{
	int error, i;

	if (sc->state.touched || sc->child != NULL)
		return (EBUSY);
	if (sc->generic.base.dmat != NULL) {
		error = bus_dma_tag_destroy(sc->generic.base.dmat);
		if (error != 0)
			return (error);
		sc->generic.base.dmat = NULL;
	}
	if (sc->physical != NULL) {
		error = bus_dma_tag_destroy(sc->physical);
		if (error != 0)
			return (error);
		sc->physical = NULL;
	}
	/* Prove that no child reservations survive before backing windows go. */
	for (i = 2; i >= 0; i--) {
		if (!sc->rman_initialized[i])
			continue;
		error = rman_fini(manager(sc, i));
		if (error != 0)
			return (error);
		sc->rman_initialized[i] = false;
	}
	for (i = 1; i >= 0; i--) {
		if (sc->generic.base.ranges[i].res != NULL) {
			error = bus_release_resource(sc->dev, SYS_RES_MEMORY, 100+i,
			    sc->generic.base.ranges[i].res);
			if (error != 0)
				return (error);
			sc->generic.base.ranges[i].res = NULL;
		}
		if (sc->range_defined[i]) {
			bus_delete_resource(sc->dev, SYS_RES_MEMORY, 100+i);
			sc->range_defined[i] = false;
		}
	}
	if (sc->iinfo_initialized) {
		ofw_bus_destroy_iinfo(&sc->generic.pci_iinfo);
		sc->iinfo_initialized = false;
	}
	if (sc->mem != NULL) {
		error = bus_release_resource(sc->dev, SYS_RES_MEMORY, 0, sc->mem);
		if (error != 0)
			return (error);
		sc->mem = NULL;
	}
	if (sc->rescal != NULL) {
		hwreset_release(sc->rescal);
		sc->rescal = NULL;
	}
	if (sc->bridge != NULL) {
		hwreset_release(sc->bridge);
		sc->bridge = NULL;
	}
	if (sc->lock_initialized) {
		mtx_destroy(&sc->config_lock);
		sc->lock_initialized = false;
	}
	return (0);
}

static int
software_setup(struct bcm2712_pcie_softc *sc)
{
	static const char * const names[] = {
	    "BCM2712 PCIe2 prefetch", "BCM2712 PCIe2 memory", "BCM2712 PCIe2 I/O"};
	struct pcie_range *range;
	struct rman *rm;
	rman_res_t existing_start, existing_size;
	int i, rid, error, domain;

	error = hwreset_get_by_ofw_name(sc->dev, sc->node, "bridge", &sc->bridge);
	if (error != 0)
		return (error);
	error = hwreset_get_by_ofw_name(sc->dev, sc->node, "rescal", &sc->rescal);
	if (error != 0)
		return (error);
	rid = 0;
	sc->mem = bus_alloc_resource_any(sc->dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL)
		return (ENXIO);
	if (rman_get_start(sc->mem) != UINT64_C(0x1000120000) ||
	    rman_get_size(sc->mem) != 0x9310)
		return (EINVAL);
	mtx_init(&sc->config_lock, "BCM2712 PCIe2 config", NULL, MTX_DEF);
	sc->lock_initialized = true;
	/* No generic setup helper may replace this checked translated tag. */
	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 1, 0,
	    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
	    UINT64_C(1)<<30, 65535, UINT64_C(1)<<30, 0, NULL, NULL, &sc->physical);
	if (error != 0)
		return (error);
	error = bus_get_domain(sc->dev, &domain);
	if (error != 0 && error != ENOENT)
		return (error);
	if (error == 0) {
		error = bus_dma_tag_set_domain(sc->physical, domain);
		if (error != 0)
			return (error);
	}
	error = bus_dma_tag_create_bcm2712_ram(sc->physical, &sc->plan.ram_policy,
	    1, 0, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, UINT64_C(1)<<30,
	    65535, UINT64_C(1)<<30, 0, NULL, NULL, &sc->generic.base.dmat);
	if (error != 0)
		return (error);
	/* pci_host_generic inherited methods consume these exact fields. */
	sc->generic.base.dev = sc->dev;
	sc->generic.base.ecam = 2;
	sc->generic.base.bus_start = 0;
	sc->generic.base.bus_end = 255;
	sc->generic.base.coherent = 0;
	sc->generic.base.quirks = PCIE_CUSTOM_CONFIG_SPACE_QUIRK;
	sc->generic.base.nranges = 2;
	sc->generic.base.has_pmem = true;
	STAILQ_INIT(&sc->generic.pci_ofw_devlist);
	for (i = 0; i < 3; i++) {
		rm = manager(sc, i);
		rm->rm_type = RMAN_ARRAY;
		rm->rm_descr = names[i];
		error = rman_init(rm);
		if (error != 0)
			return (error);
		sc->rman_initialized[i] = true;
	}
	for (i = 0; i < 2; i++) {
		range = &sc->generic.base.ranges[i];
		range->pci_base = sc->plan.outbound[i].pci;
		range->phys_base = sc->plan.outbound[i].cpu;
		range->size = sc->plan.outbound[i].length;
		range->flags = i == 0 ? FLAG_TYPE_MEM : FLAG_TYPE_PMEM;
		range->rid = 100+i;
		/* Never overwrite someone else's parent resource-list entry. */
		error = bus_get_resource(sc->dev, SYS_RES_MEMORY, 100+i,
		    &existing_start, &existing_size);
		if (error != ENOENT)
			return (error == 0 ? EEXIST : error);
		error = bus_set_resource(sc->dev, SYS_RES_MEMORY, 100+i,
		    range->phys_base, range->size);
		if (error != 0)
			return (error);
		sc->range_defined[i] = true;
		rid = 100+i;
		range->res = bus_alloc_resource_any(sc->dev, SYS_RES_MEMORY, &rid,
		    RF_ACTIVE | RF_UNMAPPED | (i == 1 ? RF_PREFETCHABLE : 0));
		if (range->res == NULL)
			return (ENXIO);
		if (rman_get_start(range->res) != range->phys_base ||
		    rman_get_size(range->res) != range->size)
			return (EINVAL);
		error = rman_manage_region(manager(sc, i == 0 ? 1 : 0),
		    range->pci_base, range->pci_base + (range->size - 1));
		if (error != 0)
			return (error);
	}
	/* The validated raw interrupt-map is consumed by inherited FDT INTx.
	 * OFW PCI child list stays empty: RP1 is not a standard PCI reg node. */
	ofw_bus_setup_iinfo(sc->node, &sc->generic.pci_iinfo, sizeof(cell_t));
	sc->iinfo_initialized = true;
	return (0);
}

static int
hardware_read(void *arg, uint32_t offset, unsigned width, uint32_t *value)
{
	struct bcm2712_pcie_softc *sc = arg;
	if ((width != 2 && width != 4) || offset % width != 0 ||
	    offset > 0x9310 - width)
		return (EINVAL);
	*value = width == 2 ? bus_read_2(sc->mem, offset) : bus_read_4(sc->mem, offset);
	return (0);
}

static int
hardware_write(void *arg, uint32_t offset, unsigned width, uint32_t value)
{
	struct bcm2712_pcie_softc *sc = arg;
	if ((width != 2 && width != 4) || offset % width != 0 ||
	    offset > 0x9310 - width)
		return (EINVAL);
	if (width == 2)
		bus_write_2(sc->mem, offset, value);
	else
		bus_write_4(sc->mem, offset, value);
	bus_barrier(sc->mem, offset, width, BUS_SPACE_BARRIER_WRITE);
	return (0);
}

static int
hardware_reset(void *arg, enum bcm2712_pcie_reset which, int asserted)
{
	struct bcm2712_pcie_softc *sc = arg;
	hwreset_t reset = which == BCM2712_PCIE_BRIDGE ? sc->bridge : sc->rescal;
	return (asserted ? hwreset_assert(reset) : hwreset_deassert(reset));
}

static void
hardware_delay(void *arg __unused, unsigned usec)
{
	DELAY(usec);
}

static int
dependencies_ready(void *arg)
{
	struct bcm2712_pcie_softc *sc = arg;
	if (OF_device_from_xref(0x27) != sc->msi ||
	    !provider_is(sc->msi, sc->msi_node, "bcm2712_mip") ||
	    OF_device_from_xref(OF_xref_from_node(sc->gpio_node)) != sc->gpio ||
	    !provider_is(sc->gpio, sc->gpio_node, "bcmstbgpio"))
		return (ENXIO);
	return (intr_msi_validate_provider(sc->msi, 0x27));
}

static int
publish_children(void *arg)
{
	struct bcm2712_pcie_softc *sc = arg;
	int error;
	mtx_lock(&sc->config_lock);
	sc->ready = true;
	mtx_unlock(&sc->config_lock);
	sc->child = device_add_child(sc->dev, "pci", DEVICE_UNIT_ANY);
	if (sc->child == NULL)
		error = ENOMEM;
	else
		error = device_probe_and_attach(sc->child);
	if (error == 0 && device_is_attached(sc->child))
		return (0);
	if (error == 0)
		error = ENXIO;
	mtx_lock(&sc->config_lock);
	sc->ready = false;
	mtx_unlock(&sc->config_lock);
	/* The touched host has no proven electrical rollback. Keep the child
	 * and all parent owners rather than detaching behind a closed config
	 * gate. Current pci_attach fails before scanning; this also bounds a
	 * future nontransactional child attach failure. */
	if (sc->child != NULL)
		device_disable(sc->child);
	return (error);
}

static const struct bcm2712_pcie_ops hardware_ops = {
	hardware_read, hardware_write, hardware_reset, hardware_delay,
	dependencies_ready, publish_children
};

static int
bcm2712_pcie_probe(device_t dev)
{
	uint32_t reg[4];
	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev,"brcm,bcm2712-pcie") ||
	    cells(ofw_bus_get_node(dev),"reg",reg,4) != 0 ||
	    reg[0] != 0x10 || reg[1] != 0x120000 || reg[2] != 0 || reg[3] != 0x9310)
		return (ENXIO);
	device_set_desc(dev,"BCM2712 selected PCIe2 root port (RP1 nexus not attached)");
	return (BUS_PROBE_DEFAULT);
}

static int
bcm2712_pcie_attach(device_t dev)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	int error, cleanup;
	sc->dev = dev;
	sc->node = ofw_bus_get_node(dev);
	error = preflight(sc);
	if (error != 0) {
		device_printf(dev,"selected DT/provider preflight refused (%d), hardware untouched\n",error);
		return (error);
	}
	error = software_setup(sc);
	if (error == 0)
		error = bcm2712_pcie_start(&sc->plan, &hardware_ops, sc, &sc->state);
	if (error == 0) {
		device_printf(dev,"PCIe2 link Gen%u x%u; external MIP, domain2, RAM aperture64GiB; RP1 peripherals not published\n",
		    sc->state.generation,sc->state.lanes);
		return (0);
	}
	if (sc->lock_initialized)
		mtx_lock(&sc->config_lock);
	sc->ready = false;
	if (sc->lock_initialized)
		mtx_unlock(&sc->config_lock);
	cleanup = software_cleanup(sc);
	if (cleanup == 0)
		return (error);
	sc->retained = true;
	device_printf(dev,"PCIe2 unavailable at phase%u (%d): retaining guarded owners (%d); no usable host claim\n",
	    sc->state.phase,error,cleanup);
	return (0);
}

/* The shared index/data pair must remain locked across the entire access. */
static uint32_t
bcm2712_pcie_read_config(device_t dev, u_int bus, u_int slot, u_int func,
    u_int reg, int width)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	uint32_t index, offset, value = UINT32_MAX;

	if (bcm2712_pcie_config_address(bus, slot, func, reg, width,
	    &index, &offset) != 0 || !sc->lock_initialized)
		return (value);
	mtx_lock(&sc->config_lock);
	if (sc->ready && (bus == 0 ||
	    (bus_read_4(sc->mem, 0x4068) & 0x30) == 0x30)) {
		if (bus != 0) {
			bus_write_4(sc->mem, 0x9000, index);
			bus_barrier(sc->mem, 0x9000, 4, BUS_SPACE_BARRIER_WRITE);
		}
		switch (width) {
		case 1: value = bus_read_1(sc->mem, offset); break;
		case 2: value = bus_read_2(sc->mem, offset); break;
		case 4: value = bus_read_4(sc->mem, offset); break;
		}
	}
	mtx_unlock(&sc->config_lock);
	return (value);
}

static void
bcm2712_pcie_write_config(device_t dev, u_int bus, u_int slot, u_int func,
    u_int reg, uint32_t value, int width)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	uint32_t index, offset;

	if (bcm2712_pcie_config_address(bus, slot, func, reg, width,
	    &index, &offset) != 0 || !sc->lock_initialized)
		return;
	mtx_lock(&sc->config_lock);
	if (sc->ready && (bus == 0 ||
	    (bus_read_4(sc->mem, 0x4068) & 0x30) == 0x30)) {
		if (bus != 0) {
			bus_write_4(sc->mem, 0x9000, index);
			bus_barrier(sc->mem, 0x9000, 4, BUS_SPACE_BARRIER_WRITE);
		}
		switch (width) {
		case 1: bus_write_1(sc->mem, offset, value); break;
		case 2: bus_write_2(sc->mem, offset, value); break;
		case 4: bus_write_4(sc->mem, offset, value); break;
		}
		bus_barrier(sc->mem, offset, width, BUS_SPACE_BARRIER_WRITE);
	}
	mtx_unlock(&sc->config_lock);
}

static struct resource *
bcm2712_pcie_alloc_resource(device_t dev, device_t child, int type, int rid,
    rman_res_t start, rman_res_t end, rman_res_t count, u_int flags)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	if (!sc->ready)
		return (NULL);
	return (pci_host_generic_core_alloc_resource(dev, child, type, rid,
	    start, end, count, flags));
}

static struct rman *
bcm2712_pcie_get_rman(device_t dev, int type, u_int flags)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	unsigned which;
	if (type == SYS_RES_MEMORY)
		which = (flags & RF_PREFETCHABLE) != 0 ? 0 : 1;
	else if (type == SYS_RES_IOPORT)
		which = 2;
	else
		return (NULL);
	/* Existing owners may release resources while the host is guarded. */
	return (sc->rman_initialized[which] ? manager(sc, which) : NULL);
}

static int
bcm2712_pcie_setup_intr(device_t dev, device_t child, struct resource *irq,
    int flags, driver_filter_t *filter, driver_intr_t *handler, void *arg,
    void **cookie)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	if (!sc->ready)
		return (ENXIO);
	return (bus_generic_setup_intr(dev, child, irq, flags, filter, handler,
	    arg, cookie));
}

static int
msi_provider(struct bcm2712_pcie_softc *sc, device_t child, bool allocate,
    phandle_t *parent)
{
	int error;
	if (allocate && !sc->ready)
		return (ENXIO);
	error = ofw_bus_msimap(sc->node, pci_get_rid(child), parent, NULL);
	if (error != 0)
		return (error);
	if (*parent != 0x27 || OF_device_from_xref(*parent) != sc->msi ||
	    !provider_is(sc->msi, sc->msi_node, "bcm2712_mip"))
		return (ENXIO);
	return (intr_msi_validate_provider(sc->msi, *parent));
}

static int
bcm2712_pcie_alloc_msi(device_t dev, device_t child, int count,
    int maxcount, int *irqs)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	phandle_t parent;
	int error = msi_provider(sc, child, true, &parent);
	return (error != 0 ? error :
	    intr_alloc_msi(dev, child, parent, count, maxcount, irqs));
}

static int
bcm2712_pcie_release_msi(device_t dev, device_t child, int count, int *irqs)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	phandle_t parent;
	int error = msi_provider(sc, child, false, &parent);
	return (error != 0 ? error :
	    intr_release_msi(dev, child, parent, count, irqs));
}

static int
bcm2712_pcie_alloc_msix(device_t dev, device_t child, int *irq)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	phandle_t parent;
	int error = msi_provider(sc, child, true, &parent);
	return (error != 0 ? error : intr_alloc_msix(dev, child, parent, irq));
}

static int
bcm2712_pcie_release_msix(device_t dev, device_t child, int irq)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	phandle_t parent;
	int error = msi_provider(sc, child, false, &parent);
	return (error != 0 ? error : intr_release_msix(dev, child, parent, irq));
}

static int
bcm2712_pcie_map_msi(device_t dev, device_t child, int irq, uint64_t *addr,
    uint32_t *data)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	phandle_t parent;
	uint64_t mapped;
	uint32_t message;
	int error;

	if (addr == NULL || data == NULL)
		return (EINVAL);
	error = msi_provider(sc, child, true, &parent);
	if (error == 0)
		error = intr_map_msi(dev, child, parent, irq, &mapped, &message);
	if (error != 0)
		return (error);
	/* Raw MIP doorbell address is never passed through the RAM DMA tag. */
	if (mapped < sc->plan.raw_msi.pci_message_addr ||
	    mapped - sc->plan.raw_msi.pci_message_addr >= sc->plan.raw_msi.length ||
	    (mapped & 3) != 0 || message >= 64)
		return (EINVAL);
	*addr = mapped;
	*data = message;
	return (0);
}

static int
bcm2712_pcie_refuse(device_t dev __unused)
{
	/* No proven electrical quiescence/rollback contract yet. */
	return (EBUSY);
}

static void
bcm2712_pcie_new_pass(device_t dev)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	if (sc->ready)
		bus_generic_new_pass(dev);
}

static void
bcm2712_pcie_driver_added(device_t dev, driver_t *driver)
{
	struct bcm2712_pcie_softc *sc = device_get_softc(dev);
	if (sc->ready)
		bus_generic_driver_added(dev, driver);
}

static device_method_t bcm2712_pcie_methods[] = {
	DEVMETHOD(device_probe, bcm2712_pcie_probe),
	DEVMETHOD(device_attach, bcm2712_pcie_attach),
	DEVMETHOD(device_detach, bcm2712_pcie_refuse),
	DEVMETHOD(device_suspend, bcm2712_pcie_refuse),
	DEVMETHOD(device_resume, bcm2712_pcie_refuse),
	DEVMETHOD(device_shutdown, bcm2712_pcie_refuse),
	DEVMETHOD(bus_new_pass, bcm2712_pcie_new_pass),
	DEVMETHOD(bus_driver_added, bcm2712_pcie_driver_added),
	DEVMETHOD(bus_alloc_resource, bcm2712_pcie_alloc_resource),
	DEVMETHOD(bus_get_rman, bcm2712_pcie_get_rman),
	DEVMETHOD(bus_setup_intr, bcm2712_pcie_setup_intr),
	DEVMETHOD(pcib_read_config, bcm2712_pcie_read_config),
	DEVMETHOD(pcib_write_config, bcm2712_pcie_write_config),
	DEVMETHOD(pcib_alloc_msi, bcm2712_pcie_alloc_msi),
	DEVMETHOD(pcib_release_msi, bcm2712_pcie_release_msi),
	DEVMETHOD(pcib_alloc_msix, bcm2712_pcie_alloc_msix),
	DEVMETHOD(pcib_release_msix, bcm2712_pcie_release_msix),
	DEVMETHOD(pcib_map_msi, bcm2712_pcie_map_msi),
	DEVMETHOD_END
};

DEFINE_CLASS_1(pcib, bcm2712_pcie_driver, bcm2712_pcie_methods,
    sizeof(struct bcm2712_pcie_softc), generic_pcie_fdt_driver);
EARLY_DRIVER_MODULE(bcm2712_pcie, simplebus, bcm2712_pcie_driver, 0, 0,
    BUS_PASS_SUPPORTDEV + BUS_PASS_ORDER_LATE);
MODULE_DEPEND(bcm2712_pcie, pci, 1, 1, 1);
