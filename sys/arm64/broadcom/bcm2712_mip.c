/*	$OpenBSD: bcm2712_mip.c,v 1.1 2025/08/21 14:53:07 kettenis Exp $	*/
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

/* FreeBSD selected MIP0 adaptation. The message target is PCI bus space,
 * not a second CPU MMIO mapping. Parent GIC sources are held for life.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <machine/bus.h>
#include <machine/intr.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include "gic_if.h"
#include "msi_if.h"

#define MIP_SLOTS 64
#define MIP_CFG_L 0x20
#define MIP_CFG_H 0x30
#define MIP_MASK_HOST_L 0x40
#define MIP_MASK_HOST_H 0x50
#define MIP_MASK_VPU_L 0x60
#define MIP_MASK_VPU_H 0x70
#define MIP_CPU_BASE UINT64_C(0x1000130000)
#define MIP_BUS_TARGET UINT64_C(0xfffffff000)

struct bcm2712_mip_softc {
	struct resource *mem;
	struct mtx lock;
	device_t parent;
	struct intr_irqsrc *sources[MIP_SLOTS];
	device_t owners[MIP_SLOTS];
	unsigned groups[MIP_SLOTS];
	uint64_t allocated;
	bool lock_initialized, claimed, ready;
};

static uint64_t
bcm2712_mip_bits(unsigned first, unsigned count)
{
	return (count == 64 ? UINT64_MAX : ((UINT64_C(1) << count) - 1) << first);
}

static int
bcm2712_mip_index(struct bcm2712_mip_softc *sc, struct intr_irqsrc *src)
{
	unsigned i;
	for (i = 0; i < MIP_SLOTS; i++)
		if (sc->sources[i] == src && src != NULL)
			return (i);
	return (-1);
}

static int
bcm2712_mip_alloc(device_t dev, device_t child, int count, int maxcount,
    device_t *pic, struct intr_irqsrc **srcs)
{
	struct bcm2712_mip_softc *sc = device_get_softc(dev);
	uint64_t bits;
	unsigned first, i;

	if (!sc->ready)
		return (ENXIO);
	if (child == NULL || count <= 0 || maxcount < count || maxcount > 64 ||
	    !powerof2(count) || !powerof2(maxcount))
		return (EINVAL);
	mtx_lock(&sc->lock);
	for (first = 0; first + count <= MIP_SLOTS; first += maxcount) {
		bits = bcm2712_mip_bits(first, count);
		if ((sc->allocated & bits) == 0) {
			sc->allocated |= bits;
			for (i = 0; i < count; i++) {
				sc->owners[first + i] = child;
				sc->groups[first + i] = i == 0 ? count : 0;
				srcs[i] = sc->sources[first + i];
			}
			*pic = sc->parent;
			mtx_unlock(&sc->lock);
			return (0);
		}
	}
	mtx_unlock(&sc->lock);
	return (ENOSPC);
}

static int
bcm2712_mip_release(device_t dev, device_t child, int count,
    struct intr_irqsrc **srcs)
{
	struct bcm2712_mip_softc *sc = device_get_softc(dev);
	int first, i, error = 0;

	if (!sc->ready)
		return (ENXIO);
	if (child == NULL || count <= 0 || count > MIP_SLOTS || !powerof2(count))
		return (EINVAL);
	mtx_lock(&sc->lock);
	first = bcm2712_mip_index(sc, srcs[0]);
	if (first < 0 || first + count > MIP_SLOTS || sc->groups[first] != (unsigned)count) {
		error = EINVAL;
		goto out;
	}
	for (i = 0; i < count; i++) {
		if (sc->sources[first + i] != srcs[i] ||
		    sc->owners[first + i] != child ||
		    !(sc->allocated & (UINT64_C(1) << (first + i)))) {
			error = EINVAL;
			goto out;
		}
		if (srcs[i]->isrc_handlers != 0) {
			error = EBUSY;
			goto out;
		}
	}
	sc->allocated &= ~bcm2712_mip_bits(first, count);
	for (i = 0; i < count; i++) {
		sc->owners[first + i] = NULL;
		sc->groups[first + i] = 0;
	}
 out:
	mtx_unlock(&sc->lock);
	return (error);
}

static int
bcm2712_mip_alloc_msix(device_t dev, device_t child, device_t *pic,
    struct intr_irqsrc **src)
{
	return (bcm2712_mip_alloc(dev, child, 1, 1, pic, src));
}

static int
bcm2712_mip_release_msix(device_t dev, device_t child, struct intr_irqsrc *src)
{
	return (bcm2712_mip_release(dev, child, 1, &src));
}

static int
bcm2712_mip_map(device_t dev, device_t child, struct intr_irqsrc *src,
    uint64_t *addr, uint32_t *data)
{
	struct bcm2712_mip_softc *sc = device_get_softc(dev);
	int index, error = 0;

	if (!sc->ready)
		return (ENXIO);
	mtx_lock(&sc->lock);
	index = bcm2712_mip_index(sc, src);
	if (index < 0 || sc->owners[index] != child || child == NULL ||
	    !(sc->allocated & (UINT64_C(1) << index)))
		error = EINVAL;
	else {
		*addr = MIP_BUS_TARGET;
		*data = index;
	}
	mtx_unlock(&sc->lock);
	return (error);
}

static int
bcm2712_mip_cleanup(device_t dev)
{
	struct bcm2712_mip_softc *sc = device_get_softc(dev);
	int error;
	/* No parent unreserve/provider unregister exists. Preserve every owner. */
	if (sc->claimed)
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
bcm2712_mip_probe(device_t dev)
{
	pcell_t reg[8];
	phandle_t node = ofw_bus_get_node(dev);
	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "brcm,bcm2712-mip") ||
	    OF_getencprop(node, "reg", reg, sizeof(reg)) != sizeof(reg) ||
	    reg[0] != 0x10 || reg[1] != 0x130000)
		return (ENXIO);
	device_set_desc(dev, "BCM2712 MIP0 MSI controller");
	return (BUS_PROBE_DEFAULT);
}

static int
bcm2712_mip_attach(device_t dev)
{
	struct bcm2712_mip_softc *sc = device_get_softc(dev);
	phandle_t node = ofw_bus_get_node(dev), parent, xref;
	device_t owner;
	pcell_t reg[8], ranges[5], offset, cells;
	static const unsigned registers[] = { MIP_MASK_VPU_L, MIP_MASK_VPU_H,
	    MIP_CFG_L, MIP_CFG_H, MIP_MASK_HOST_L, MIP_MASK_HOST_H };
	unsigned i;
	int length, error, rid = 0;

	/* Selected D0 MIP0 only. The second reg tuple is raw PCI message space. */
	if (OF_getproplen(node, "reg") != sizeof(reg) ||
	    OF_getencprop(node, "reg", reg, sizeof(reg)) != sizeof(reg) ||
	    reg[0] != 0x10 || reg[1] != 0x130000 || reg[2] != 0 || reg[3] != 0xc0 ||
	    reg[4] != 0xff || reg[5] != 0xfffff000 || reg[6] != 0 || reg[7] != 0x1000 ||
	    !OF_hasprop(node, "msi-controller") ||
	    OF_getproplen(node, "msi-controller") != 0 ||
	    OF_getproplen(node, "msi-ranges") != sizeof(ranges) ||
	    OF_getencprop(node, "msi-ranges", ranges, sizeof(ranges)) != sizeof(ranges) ||
	    ranges[1] != 0 || ranges[2] != 128 || ranges[3] != 1 || ranges[4] != 64)
		return (EINVAL);
	length = OF_getproplen(node, "brcm,msi-offset");
	offset = 0;
	if (length != -1 && (length != sizeof(offset) ||
	    OF_getencprop(node, "brcm,msi-offset", &offset, sizeof(offset)) != sizeof(offset)))
		return (EINVAL);
	if (offset != 0)
		return (EOPNOTSUPP);
	length = OF_getproplen(node, "#msi-cells");
	if (length != -1 && (length != sizeof(cells) ||
	    OF_getencprop(node, "#msi-cells", &cells, sizeof(cells)) != sizeof(cells) ||
	    cells != 0))
		return (EINVAL);
	parent = OF_node_from_xref(ranges[0]);
	if (parent <= 0 || OF_getproplen(parent, "#interrupt-cells") != sizeof(cells) ||
	    OF_getencprop(parent, "#interrupt-cells", &cells,
	    sizeof(cells)) != sizeof(cells) || cells != 3)
		return (EINVAL);
	sc->parent = OF_device_from_xref(ranges[0]);
	if (sc->parent == NULL || !ofw_bus_is_compatible(sc->parent, "arm,gic-400"))
		return (ENXIO);
	/* simplebus does not publish OFW device identities.  Do not overwrite
	 * an already owned xref, and do this before claiming/writing hardware. */
	xref = OF_xref_from_node(node);
	if (xref <= 0)
		return (EINVAL);
	owner = OF_device_from_xref(xref);
	if (owner != NULL && owner != dev)
		return (EEXIST);
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL)
		return (ENXIO);
	if (rman_get_start(sc->mem) != MIP_CPU_BASE || rman_get_size(sc->mem) != 0xc0) {
		error = EINVAL;
		goto fail;
	}
	mtx_init(&sc->lock, device_get_nameunit(dev), NULL, MTX_DEF);
	sc->lock_initialized = true;
	/* DT SPI128 is architectural INTID160, not INTID128. */
	error = GIC_RESERVE_MSI_RANGE_CHECKED(sc->parent, 160, MIP_SLOTS);
	if (error != 0)
		goto fail;
	sc->claimed = true;
	/* Hold the complete range: children allocate only our local bitmap.
	 * Individual parent allocation avoids a false 64-vector alignment demand.
	 */
	for (i = 0; i < MIP_SLOTS; i++) {
		error = GIC_ALLOC_MSIX(sc->parent, 160, MIP_SLOTS, &sc->sources[i]);
		if (error != 0)
			goto fail;
	}
	for (i = 0; i < nitems(registers); i++) {
		uint32_t value = i < 4 ? UINT32_MAX : 0;
		bus_write_4(sc->mem, registers[i], value);
		bus_barrier(sc->mem, registers[i], 4, BUS_SPACE_BARRIER_WRITE);
		if (bus_read_4(sc->mem, registers[i]) != value) {
			error = EIO;
			goto fail;
		}
	}
	/* Publish only fully usable state; registration makes callbacks visible. */
	sc->ready = true;
	error = intr_msi_register(dev, xref);
	if (error != 0) {
		sc->ready = false;
		goto fail;
	}
	/* The PIC registry is already live.  Keep its usable owner if a later
	 * identity publication fails; no unregister/unreserve API exists. */
	owner = OF_device_from_xref(xref);
	if (owner != NULL && owner != dev)
		error = EEXIST;
	else
		error = OF_device_register_xref(xref, dev);
	if (error != 0 || OF_device_from_xref(xref) != dev) {
		device_printf(dev, "OFW identity unavailable: retaining registered MSI owner (%d)\n",
		    error != 0 ? error : EIO);
		return (0);
	}
	return (0);
 fail:
	if (bcm2712_mip_cleanup(dev) == 0)
		return (error);
	device_printf(dev, "MIP unavailable: retaining unpublished parent/MSI owners (%d)\n", error);
	return (0);
}

static device_method_t bcm2712_mip_methods[] = {
	DEVMETHOD(device_probe, bcm2712_mip_probe),
	DEVMETHOD(device_attach, bcm2712_mip_attach),
	DEVMETHOD(device_detach, bcm2712_mip_cleanup),
	DEVMETHOD(msi_alloc_msi, bcm2712_mip_alloc),
	DEVMETHOD(msi_release_msi, bcm2712_mip_release),
	DEVMETHOD(msi_alloc_msix, bcm2712_mip_alloc_msix),
	DEVMETHOD(msi_release_msix, bcm2712_mip_release_msix),
	DEVMETHOD(msi_map_msi, bcm2712_mip_map),
	DEVMETHOD_END
};
static driver_t bcm2712_mip_driver = {
	"bcm2712_mip", bcm2712_mip_methods, sizeof(struct bcm2712_mip_softc)
};
EARLY_DRIVER_MODULE(bcm2712_mip, simplebus, bcm2712_mip_driver, NULL, NULL,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(bcm2712_mip, 1);
