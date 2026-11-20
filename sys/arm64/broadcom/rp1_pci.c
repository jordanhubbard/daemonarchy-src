/*	$OpenBSD: rpone.c,v 1.1 2025/08/20 21:40:34 kettenis Exp $ */

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

/* FreeBSD RP1 PCI/FDT nexus and interrupt-controller adaptation. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/rman.h>
#include <machine/bus.h>
#include <machine/intr.h>
#include <machine/resource.h>
#include <dev/fdt/simplebus.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include "pic_if.h"

#define RP1_IRQS 61
#define RP1_PERIPH_SIZE 0x400000
#define RP1_SRAM_SIZE 0x10000
#define RP1_CFG(n) (0x108008 + 4 * (n))
#define RP1_SET 0x800
#define RP1_CLR 0xc00
#define RP1_ENABLE 1
#define RP1_ACK 4
#define RP1_ACK_ENABLE 8

struct rp1_softc;
struct rp1_irq {
	struct intr_irqsrc isrc;
	struct rp1_softc *sc;
	struct resource *parent_irq;
	void *cookie;
	u_int vector, trigger;
	bool registered;
};
struct rp1_softc {
	struct simplebus_softc simple;
	device_t dev;
	phandle_t node;
	struct resource *bar[2];
	struct rman memory;
	struct rp1_irq irq[RP1_IRQS];
	uint64_t child_base, child_size;
	bool manager, msix, published, busmaster, registers_valid;
};

/* Keep shared SRAM in BAR2 separate, even if firmware placed BARs adjacent. */
static int
rp1_window(uint64_t address, uint64_t size, uint64_t base, uint64_t span,
    unsigned *bar, uint64_t *offset)
{
	uint64_t relative;

	if (size == 0 || address < base)
		return (EINVAL);
	relative = address - base;
	if (relative >= span || size > span - relative)
		return (ERANGE);
	if (relative < RP1_PERIPH_SIZE) {
		if (size > RP1_PERIPH_SIZE - relative)
			return (ERANGE);
		*bar = 0;
		*offset = relative;
	} else {
		relative -= RP1_PERIPH_SIZE;
		if (relative >= RP1_SRAM_SIZE || size > RP1_SRAM_SIZE - relative)
			return (ERANGE);
		*bar = 1;
		*offset = relative;
	}
	return (0);
}

static int
rp1_dma_layout(const uint32_t dma[21], const uint32_t range[7])
{
	static const uint32_t ram[7] = {
		0x10, 0, 0x43000000, 0x10, 0, 0x10, 0
	};
	static const uint32_t alias[7] = {
		0, 0, 0x02000000, 0x10, 0, 0x10, 0
	};

	if (memcmp(dma, ram, sizeof(ram)) != 0 ||
	    memcmp(dma + 7, range, 7 * sizeof(*range)) != 0 ||
	    memcmp(dma + 14, alias, sizeof(alias)) != 0)
		return (EINVAL);
	return (0);
}

static void
rp1_config(struct rp1_irq *irq, unsigned alias, uint32_t bits)
{
	struct resource *mem = irq->sc->bar[0];

	bus_write_4(mem, RP1_CFG(irq->vector) + alias, bits);
	/* Complete the posted PCIe write before returning to the parent PIC. */
	(void)bus_read_4(mem, RP1_CFG(irq->vector));
}

static void
rp1_disable(device_t dev, struct intr_irqsrc *isrc)
{
	struct rp1_irq *irq = (struct rp1_irq *)isrc;

	KASSERT(irq->sc == device_get_softc(dev), ("foreign RP1 IRQ"));
	rp1_config(irq, RP1_CLR, RP1_ENABLE);
}

static void
rp1_enable(device_t dev, struct intr_irqsrc *isrc)
{
	struct rp1_irq *irq = (struct rp1_irq *)isrc;

	KASSERT(irq->sc == device_get_softc(dev), ("foreign RP1 IRQ"));
	rp1_config(irq, RP1_SET, RP1_ENABLE);
}

static void
rp1_pre_thread(device_t dev, struct intr_irqsrc *isrc)
{

	/* Leave the level message unacknowledged while its thread runs. */
	rp1_disable(dev, isrc);
}

static void
rp1_post_filter(device_t dev, struct intr_irqsrc *isrc)
{
	struct rp1_irq *irq = (struct rp1_irq *)isrc;

	KASSERT(irq->sc == device_get_softc(dev), ("foreign RP1 IRQ"));
	arm_irq_memory_barrier(0);
	if (irq->trigger == 4)
		rp1_config(irq, RP1_SET, RP1_ACK);
}

static void
rp1_post_thread(device_t dev, struct intr_irqsrc *isrc)
{

	rp1_post_filter(dev, isrc);
	rp1_enable(dev, isrc);
}

static int
rp1_filter(void *arg)
{
	struct rp1_irq *irq = arg;

	if (intr_isrc_dispatch(&irq->isrc, curthread->td_intr_frame) != 0) {
		rp1_config(irq, RP1_CLR, RP1_ENABLE);
		if (irq->trigger == 4)
			rp1_config(irq, RP1_SET, RP1_ACK);
	}
	return (FILTER_HANDLED);
}

static int
rp1_decode(struct intr_map_data *data, u_int *vector, u_int *trigger)
{
	struct intr_map_data_fdt *fdt;

	if (data == NULL || data->type != INTR_MAP_DATA_FDT)
		return (EINVAL);
	fdt = (struct intr_map_data_fdt *)data;
	if (fdt->ncells != 2 || fdt->cells[0] >= RP1_IRQS ||
	    (fdt->cells[1] != 1 && fdt->cells[1] != 4))
		return (EINVAL);
	*vector = fdt->cells[0];
	*trigger = fdt->cells[1];
	return (0);
}

static int
rp1_map(device_t dev, struct intr_map_data *data, struct intr_irqsrc **isrc)
{
	struct rp1_softc *sc = device_get_softc(dev);
	u_int vector, trigger;
	int error;

	error = rp1_decode(data, &vector, &trigger);
	if (error == 0)
		*isrc = &sc->irq[vector].isrc;
	return (error);
}

static int
rp1_setup(device_t dev, struct intr_irqsrc *isrc, struct resource *res,
    struct intr_map_data *data)
{
	struct rp1_softc *sc = device_get_softc(dev);
	struct rp1_irq *irq = (struct rp1_irq *)isrc;
	u_int vector, trigger;
	int error;

	error = rp1_decode(data, &vector, &trigger);
	if (error != 0)
		return (error);
	if (isrc != &sc->irq[vector].isrc ||
	    (isrc->isrc_handlers != 0 && irq->trigger != trigger))
		return (EINVAL);
	if (isrc->isrc_handlers != 0)
		return (0);
	irq->trigger = trigger;
	rp1_disable(dev, isrc);
	rp1_config(irq, trigger == 4 ? RP1_SET : RP1_CLR, RP1_ACK_ENABLE);
	if (trigger == 4)
		rp1_config(irq, RP1_SET, RP1_ACK);
	return (0);
}

static int
rp1_teardown(device_t dev, struct intr_irqsrc *isrc, struct resource *res,
    struct intr_map_data *data)
{
	struct rp1_irq *irq = (struct rp1_irq *)isrc;

	if (isrc->isrc_handlers == 0) {
		rp1_disable(dev, isrc);
		rp1_config(irq, RP1_CLR, RP1_ACK_ENABLE);
		irq->trigger = 0;
	}
	return (0);
}

static struct resource *
rp1_alloc(device_t dev, device_t child, int type, int rid,
    rman_res_t start, rman_res_t end, rman_res_t count, u_int flags)
{
	struct rp1_softc *sc = device_get_softc(dev);
	struct simplebus_devinfo *di;
	struct resource_list_entry *entry;
	struct resource *res;
	uint64_t offset;
	unsigned bar;

	if (RMAN_IS_DEFAULT_RANGE(start, end)) {
		if (device_get_parent(child) != dev)
			return (NULL);
		di = device_get_ivars(child);
		entry = resource_list_find(&di->rl, type, rid);
		if (entry == NULL)
			return (NULL);
		start = entry->start;
		end = entry->end;
		count = entry->count;
	}
	if (type == SYS_RES_IRQ)
		return (bus_generic_alloc_resource(dev, child, type, rid,
		    start, end, count, flags));
	if (type != SYS_RES_MEMORY || (flags & RF_UNMAPPED) != 0 ||
	    count == 0 || end < start || count - 1 != end - start ||
	    rp1_window(start, count, sc->child_base, sc->child_size,
	    &bar, &offset) != 0 || sc->bar[bar] == NULL ||
	    offset > rman_get_size(sc->bar[bar]) ||
	    count > rman_get_size(sc->bar[bar]) - offset)
		return (NULL);
	start = rman_get_start(sc->bar[bar]) + offset;
	res = rman_reserve_resource(&sc->memory, start, start + count - 1,
	    count, flags & ~RF_ACTIVE, child);
	if (res == NULL)
		return (NULL);
	rman_set_type(res, type);
	rman_set_rid(res, rid);
	if ((flags & RF_ACTIVE) != 0 && bus_activate_resource(child, res) != 0) {
		rman_release_resource(res);
		return (NULL);
	}
	return (res);
}

static int
rp1_activate(device_t dev, device_t child, struct resource *res)
{
	struct rp1_softc *sc = device_get_softc(dev);
	bus_space_handle_t handle;
	rman_res_t start, size, offset;
	unsigned bar;
	int error;

	if (rman_get_type(res) != SYS_RES_MEMORY)
		return (bus_generic_activate_resource(dev, child, res));
	if (!rman_is_region_manager(res, &sc->memory) ||
	    rman_get_device(res) != child)
		return (EINVAL);
	start = rman_get_start(res);
	size = rman_get_size(res);
	for (bar = 0; bar < 2; bar++) {
		if (sc->bar[bar] == NULL || start < rman_get_start(sc->bar[bar]))
			continue;
		offset = start - rman_get_start(sc->bar[bar]);
		if (offset >= rman_get_size(sc->bar[bar]) ||
		    size > rman_get_size(sc->bar[bar]) - offset)
			continue;
		error = bus_space_subregion(rman_get_bustag(sc->bar[bar]),
		    rman_get_bushandle(sc->bar[bar]), offset, size, &handle);
		if (error != 0)
			return (error);
		rman_set_bustag(res, rman_get_bustag(sc->bar[bar]));
		rman_set_bushandle(res, handle);
		if (rman_get_virtual(sc->bar[bar]) != NULL)
			rman_set_virtual(res, (char *)rman_get_virtual(sc->bar[bar]) + offset);
		return (rman_activate_resource(res));
	}
	return (ERANGE);
}

static int
rp1_deactivate(device_t dev, device_t child, struct resource *res)
{
	struct rp1_softc *sc = device_get_softc(dev);

	if (rman_get_type(res) != SYS_RES_MEMORY)
		return (bus_generic_deactivate_resource(dev, child, res));
	if (!rman_is_region_manager(res, &sc->memory) ||
	    rman_get_device(res) != child)
		return (EINVAL);
	return (rman_deactivate_resource(res));
}

static int
rp1_release(device_t dev, device_t child, struct resource *res)
{
	struct rp1_softc *sc = device_get_softc(dev);
	int error;

	if (rman_get_type(res) != SYS_RES_MEMORY)
		return (bus_generic_release_resource(dev, child, res));
	if (!rman_is_region_manager(res, &sc->memory) ||
	    rman_get_device(res) != child)
		return (EINVAL);
	if ((rman_get_flags(res) & RF_ACTIVE) != 0) {
		error = rp1_deactivate(dev, child, res);
		if (error != 0)
			return (error);
	}
	return (rman_release_resource(res));
}

static int
rp1_adjust(device_t dev, device_t child, struct resource *res,
    rman_res_t start, rman_res_t end)
{

	if (rman_get_type(res) == SYS_RES_MEMORY)
		return (EOPNOTSUPP);
	return (bus_generic_adjust_resource(dev, child, res, start, end));
}

static int
rp1_map_resource(device_t dev, device_t child, struct resource *res,
    struct resource_map_request *args, struct resource_map *map)
{

	/* Memory children use RF_ACTIVE subregions of the existing BAR map. */
	if (rman_get_type(res) == SYS_RES_MEMORY)
		return (EOPNOTSUPP);
	return (bus_generic_map_resource(dev, child, res, args, map));
}

static int
rp1_unmap_resource(device_t dev, device_t child, struct resource *res,
    struct resource_map *map)
{

	if (rman_get_type(res) == SYS_RES_MEMORY)
		return (EOPNOTSUPP);
	return (bus_generic_unmap_resource(dev, child, res, map));
}

static bus_dma_tag_t
rp1_dma_tag(device_t dev, device_t child)
{

	/* Preserve the BCM2712 host's translated RAM DMA tag. */
	return (bus_get_dma_tag(dev));
}

static int
rp1_cleanup(device_t dev)
{
	struct rp1_softc *sc = device_get_softc(dev);
	struct rp1_irq *irq;
	int error, i;

	/* The published static PIC and its mappings are held for boot lifetime. */
	if (sc->published)
		return (EBUSY);
	for (i = RP1_IRQS - 1; i >= 0; i--) {
		irq = &sc->irq[i];
		if (sc->registers_valid)
			rp1_config(irq, RP1_CLR, RP1_ENABLE | RP1_ACK_ENABLE);
		if (irq->cookie != NULL) {
			error = bus_teardown_intr(dev, irq->parent_irq, irq->cookie);
			if (error != 0)
				return (error);
			irq->cookie = NULL;
		}
		if (irq->parent_irq != NULL) {
			error = bus_release_resource(dev, SYS_RES_IRQ, i + 1, irq->parent_irq);
			if (error != 0)
				return (error);
			irq->parent_irq = NULL;
		}
		if (irq->registered) {
			error = intr_isrc_deregister(&irq->isrc);
			if (error != 0)
				return (error);
			irq->registered = false;
		}
	}
	if (sc->msix) {
		error = pci_release_msi(dev);
		if (error != 0)
			return (error);
		sc->msix = false;
	}
	if (sc->manager) {
		error = rman_fini(&sc->memory);
		if (error != 0)
			return (error);
		sc->manager = false;
	}
	for (i = 1; i >= 0; i--) {
		if (sc->bar[i] == NULL)
			continue;
		error = bus_release_resource(dev, SYS_RES_MEMORY, PCIR_BAR(i + 1), sc->bar[i]);
		if (error != 0)
			return (error);
		sc->bar[i] = NULL;
		if (i == 0)
			sc->registers_valid = false;
	}
	if (sc->busmaster) {
		pci_disable_busmaster(dev);
		sc->busmaster = false;
	}
	return (0);
}

static int
rp1_probe(device_t dev)
{

	if (pci_get_vendor(dev) != 0x1de4 || pci_get_device(dev) != 1)
		return (ENXIO);
	device_set_desc(dev, "Raspberry Pi RP1 peripheral nexus");
	return (BUS_PROBE_DEFAULT);
}

static int
rp1_attach(device_t dev)
{
	struct rp1_softc *sc = device_get_softc(dev);
	phandle_t node, host, candidate = 0;
	pcell_t range[7], dma[21], cells;
	int count, error, i, rid;

	sc->dev = dev;
	for (i = 0; i < RP1_IRQS; i++) {
		sc->irq[i].sc = sc;
		sc->irq[i].vector = i;
	}
	/* The selected firmware node has no PCI reg/compatible property. */
	host = ofw_bus_get_node(device_get_parent(device_get_parent(dev)));
	if (host <= 0 || !ofw_bus_node_is_compatible(host, "brcm,bcm2712-pcie"))
		return (ENXIO);
	for (node = OF_child(host); node > 0; node = OF_peer(node)) {
		char name[16];
		if (!ofw_bus_node_status_okay(node) ||
		    OF_getprop(node, "name", name, sizeof(name)) <= 0)
			continue;
		name[sizeof(name) - 1] = 0;
		if (strcmp(name, "rp1") != 0)
			continue;
		if (candidate != 0)
			return (EINVAL);
		candidate = node;
	}
	if (candidate == 0)
		return (ENXIO);
	sc->node = candidate;
	if (OF_getproplen(candidate, "interrupt-controller") != 0 ||
	    OF_getencprop(candidate, "#interrupt-cells", &cells, sizeof(cells)) != sizeof(cells) || cells != 2 ||
	    OF_getencprop(candidate, "interrupt-parent", &cells, sizeof(cells)) != sizeof(cells) ||
	    OF_node_from_xref(cells) != candidate)
		return (EINVAL);
	if (OF_getencprop(candidate, "#address-cells", &cells, sizeof(cells)) != sizeof(cells) || cells != 2 ||
	    OF_getencprop(candidate, "#size-cells", &cells, sizeof(cells)) != sizeof(cells) || cells != 2 ||
	    OF_getproplen(candidate, "ranges") != sizeof(range) ||
	    OF_getencprop(candidate, "ranges", range, sizeof(range)) != sizeof(range) ||
	    range[2] != 0x02000000 || range[3] != 0 || range[4] != 0 || range[5] != 0 ||
	    (range[6] != RP1_PERIPH_SIZE && range[6] != RP1_PERIPH_SIZE + RP1_SRAM_SIZE))
		return (EINVAL);
	sc->child_base = ((uint64_t)range[0] << 32) | range[1];
	sc->child_size = range[6];
	if (sc->child_base > UINT64_MAX - (sc->child_size - 1))
		return (EINVAL);
	/*
	 * The host tag emits 0x10_00000000 + RAM physical addresses.  Only
	 * reuse it for the selected firmware layout with the matching identity
	 * window; neither the peripheral alias nor the zero-based alias may
	 * change the RAM translation.  Reject other layouts before any MMIO.
	 */
	if (OF_getproplen(candidate, "dma-ranges") != sizeof(dma) ||
	    OF_getencprop(candidate, "dma-ranges", dma, sizeof(dma)) != sizeof(dma) ||
	    rp1_dma_layout(dma, range) != 0)
		return (EINVAL);
	for (i = 0; i < 2; i++) {
		rid = PCIR_BAR(i + 1);
		sc->bar[i] = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
		if (sc->bar[i] == NULL || rman_get_size(sc->bar[i]) <
		    (i == 0 ? RP1_PERIPH_SIZE : RP1_SRAM_SIZE)) {
			error = ENXIO;
			goto fail;
		}
	}
	sc->registers_valid = true;
	sc->memory.rm_type = RMAN_ARRAY;
	sc->memory.rm_descr = "RP1 peripheral BAR subregions";
	error = rman_init(&sc->memory);
	if (error != 0)
		goto fail;
	sc->manager = true;
	for (i = 0; i < 2; i++) {
		error = rman_manage_region(&sc->memory, rman_get_start(sc->bar[i]), rman_get_end(sc->bar[i]));
		if (error != 0)
			goto fail;
	}
	count = RP1_IRQS;
	error = pci_alloc_msix(dev, &count);
	if (error != 0)
		goto fail;
	sc->msix = true;
	if (count != RP1_IRQS) {
		error = ENOSPC;
		goto fail;
	}
	for (i = 0; i < RP1_IRQS; i++) {
		rp1_config(&sc->irq[i], RP1_CLR, RP1_ENABLE | RP1_ACK_ENABLE);
		error = intr_isrc_register(&sc->irq[i].isrc, dev, 0, "%s,%d", device_get_nameunit(dev), i);
		if (error != 0)
			goto fail;
		sc->irq[i].registered = true;
		rid = i + 1;
		sc->irq[i].parent_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid, RF_ACTIVE);
		if (sc->irq[i].parent_irq == NULL) {
			error = ENXIO;
			goto fail;
		}
		error = bus_setup_intr(dev, sc->irq[i].parent_irq, INTR_TYPE_MISC | INTR_MPSAFE,
		    rp1_filter, NULL, &sc->irq[i], &sc->irq[i].cookie);
		if (error != 0)
			goto fail;
	}
	error = pci_enable_busmaster(dev);
	if (error != 0)
		goto fail;
	sc->busmaster = true;
	if (intr_pic_register(dev, OF_xref_from_node(candidate)) == NULL) {
		error = ENXIO;
		goto fail;
	}
	sc->published = true;
	simplebus_init(dev, candidate);
	for (node = OF_child(candidate); node > 0; node = OF_peer(node))
		simplebus_add_device(dev, node, 0, NULL, -1, NULL);
	bus_identify_children(dev);
	bus_attach_children(dev);
	return (0);
fail:
	if (rp1_cleanup(dev) != 0) {
		/* Retain the softc while a parent callback can still reference it. */
		device_printf(dev, "attach failed (%d); retained disabled nexus after cleanup failure\n", error);
		return (0);
	}
	return (error);
}

static device_method_t rp1_methods[] = {
	DEVMETHOD(device_probe, rp1_probe),
	DEVMETHOD(device_attach, rp1_attach),
	DEVMETHOD(device_detach, rp1_cleanup),
	DEVMETHOD(bus_alloc_resource, rp1_alloc),
	DEVMETHOD(bus_activate_resource, rp1_activate),
	DEVMETHOD(bus_deactivate_resource, rp1_deactivate),
	DEVMETHOD(bus_release_resource, rp1_release),
	DEVMETHOD(bus_get_dma_tag, rp1_dma_tag),
	DEVMETHOD(bus_adjust_resource, rp1_adjust),
	DEVMETHOD(bus_map_resource, rp1_map_resource),
	DEVMETHOD(bus_unmap_resource, rp1_unmap_resource),
	DEVMETHOD(pic_disable_intr, rp1_disable),
	DEVMETHOD(pic_enable_intr, rp1_enable),
	DEVMETHOD(pic_map_intr, rp1_map),
	DEVMETHOD(pic_setup_intr, rp1_setup),
	DEVMETHOD(pic_teardown_intr, rp1_teardown),
	DEVMETHOD(pic_pre_ithread, rp1_pre_thread),
	DEVMETHOD(pic_post_ithread, rp1_post_thread),
	DEVMETHOD(pic_post_filter, rp1_post_filter),
	DEVMETHOD_END
};
DEFINE_CLASS_1(rp1, rp1_driver, rp1_methods, sizeof(struct rp1_softc), simplebus_driver);
EARLY_DRIVER_MODULE(rp1, pci, rp1_driver, 0, 0, BUS_PASS_BUS + BUS_PASS_ORDER_LATE);
MODULE_DEPEND(rp1, pci, 1, 1, 1);
