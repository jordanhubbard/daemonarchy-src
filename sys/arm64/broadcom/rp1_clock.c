/* $OpenBSD: rpiclock.c,v 1.1 2025/09/17 09:23:43 kettenis Exp $ */

/*
 * Copyright (c) 2025 Marcus Glocker <mglocker@openbsd.org>
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


/* FreeBSD clock framework adaptation, with checked assignment/publication. */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <dev/clk/clk.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include "rp1_clock_policy.h"

#define RP1_CLOCK_XOSC 1023
#define RP1_CLOCK_ENABLE (1U << 11)
#define RP1_CLOCK_AUX (0x1fU << 5)
#define RP1_CLOCK_SRC 1U
#define RP1_PLL_LOCK (1U << 31)
#define RP1_PLL_DSMPD (1U << 2)
#define RP1_PLL_SECONDARY_RESET (1U << 16)
#define RP1_CLOCK_ASSIGNMENTS 32

struct rp1_clock_def {
	int id;
	enum rp1_clock_kind kind;
	uint32_t reg, divider, selected, maximum;
	uint64_t ceiling;
	int parents[2];
	unsigned mux[2], nparents;
	bool auxiliary;
};
static const struct rp1_clock_def rp1_clocks[] = {
	{ 0, RP1_CORE, 0x8000, 0, 0, 0, 2000000000, { RP1_CLOCK_XOSC }, { 0 }, 1, false },
	{ 1, RP1_CORE, 0xc000, 0, 0, 0, 2000000000, { RP1_CLOCK_XOSC }, { 0 }, 1, false },
	{ 3, RP1_PRIMARY, 0x8010, 0, 0, 0, 0, { 0 }, { 0 }, 1, false },
	{ 4, RP1_PRIMARY, 0xc010, 0, 0, 0, 0, { 1 }, { 0 }, 1, false },
	{ 6, RP1_HALF, 0x8010, 0, 0, 0, 0, { 3 }, { 0 }, 1, false },
	{ 8, RP1_HALF, 0xc010, 0, 0, 0, 0, { 4 }, { 0 }, 1, false },
	{ 9, RP1_SECONDARY, 0x8014, 0, 0, 0, 0, { 0 }, { 0 }, 1, false },
	{ 10, RP1_SECONDARY, 0xc014, 0, 0, 0, 0, { 1 }, { 0 }, 1, false },
	{ 12, RP1_LEAF, 0x14, 0x18, 0x20, 0xffffff, 200000000, { RP1_CLOCK_XOSC, 3 }, { 0, 2 }, 2, false },
	{ 13, RP1_LEAF, 0x24, 0x28, 0x30, 255, 50000000, { RP1_CLOCK_XOSC }, { 0 }, 1, false },
	{ 16, RP1_LEAF, 0x64, 0x68, 0x70, 255, 125000000, { 9, 3 }, { 0, 1 }, 2, true },
	{ 29, RP1_LEAF, 0x134, 0x138, 0x140, 255, 50000000, { RP1_CLOCK_XOSC }, { 0 }, 1, true },
	{ 31, RP1_LEAF, 0x154, 0x158, 0x160, 255, 1000000, { RP1_CLOCK_XOSC }, { 0 }, 1, true },
	{ 32, RP1_LEAF, 0x164, 0x168, 0x170, 255, 200000000, { 3 }, { 0 }, 1, true },
};
#define RP1_CLOCK_COUNT nitems(rp1_clocks)
struct rp1_clock_softc {
	struct resource *mem;
	struct mtx lock;
	clk_t xosc;
	uint64_t xosc_frequency;
	struct clkdom *domain;
	struct clknode *nodes[RP1_CLOCK_COUNT];
	char names[RP1_CLOCK_COUNT][48];
	const char *parents[RP1_CLOCK_COUNT][2];
	uint32_t assigned[RP1_CLOCK_ASSIGNMENTS], rates[RP1_CLOCK_ASSIGNMENTS];
	unsigned nassigned;
	bool lock_initialized, ready, poisoned;
};
struct rp1_clknode_softc {
	struct rp1_clock_softc *provider;
	const struct rp1_clock_def *def;
};

static int
rp1_clock_index(int id)
{

	for (unsigned i = 0; i < RP1_CLOCK_COUNT; i++)
		if (rp1_clocks[i].id == id)
			return (i);
	return (-1);
}

static uint32_t
rp1_clock_read(struct rp1_clock_softc *sc, unsigned reg)
{

	return (bus_read_4(sc->mem, reg));
}

static void
rp1_clock_write(struct rp1_clock_softc *sc, unsigned reg, uint32_t value)
{

	bus_write_4(sc->mem, reg, value);
	(void)bus_read_4(sc->mem, reg);
}

static int
rp1_clock_wait(struct rp1_clock_softc *sc, unsigned reg, uint32_t mask,
    uint32_t expected)
{

	for (unsigned i = 0; i < 1000; i++) {
		if ((rp1_clock_read(sc, reg) & mask) == expected)
			return (0);
		DELAY(100);
	}
	sc->ready = false;
	sc->poisoned = true;
	return (ETIMEDOUT);
}

static int
rp1_clock_parent(struct rp1_clock_softc *sc, const struct rp1_clock_def *def)
{
	uint32_t ctrl, selected, mux;

	if (def->kind != RP1_LEAF)
		return (0);
	ctrl = rp1_clock_read(sc, def->reg);
	if (def->auxiliary)
		mux = (ctrl & RP1_CLOCK_AUX) >> 5;
	else {
		selected = rp1_clock_read(sc, def->selected);
		if (selected != 0 && (selected & (selected - 1)) != 0)
			return (-1);
		mux = selected == 0 ? ctrl & (def->id == 12 ? 3U : 1U) : (unsigned)ffs(selected) - 1;
	}
	for (unsigned i = 0; i < def->nparents; i++)
		if (def->mux[i] == mux)
			return (i);
	return (-1);
}

static int
rp1_clknode_init(struct clknode *clk, device_t dev)
{
	struct rp1_clknode_softc *node = clknode_get_softc(clk);
	int parent;

	parent = rp1_clock_parent(node->provider, node->def);
	if (parent < 0)
		return (ENXIO);
	clknode_init_parent_idx(clk, parent);
	return (0);
}

static int
rp1_clknode_recalc(struct clknode *clk, uint64_t *frequency)
{
	struct rp1_clknode_softc *node = clknode_get_softc(clk);
	struct rp1_clock_softc *sc = node->provider;
	const struct rp1_clock_def *def = node->def;
	uint64_t value;
	uint32_t a, b;
	int error = 0;

	mtx_lock(&sc->lock);
	/*
	 * The framework refreshes every descendant after a parent changes,
	 * including clocks not assigned yet, and panics on recalc errors.
	 * Represent unconfigured/unavailable outputs as zero Hz.  Publication
	 * remains blocked until all required rates have been checked.
	 */
	if (sc->poisoned) {
		*frequency = 0;
		goto out;
	}
	a = rp1_clock_read(sc, def->reg);
	switch (def->kind) {
	case RP1_CORE:
		if ((a & 0x3f) != 1 || *frequency > 100000000) {
			*frequency = 0;
			break;
		}
		value = (uint64_t)(rp1_clock_read(sc, def->reg + 8) & 0xfff) << 24;
		if ((rp1_clock_read(sc, def->reg + 4) & RP1_PLL_DSMPD) == 0)
			value |= rp1_clock_read(sc, def->reg + 12) & 0xffffff;
		*frequency = (*frequency * value + (1U << 23)) >> 24;
		break;
	case RP1_PRIMARY:
		a = ((a >> 16) & 7) * ((a >> 12) & 7);
		if (a == 0)
			*frequency = 0;
		else
			*frequency = (*frequency + a / 2) / a;
		break;
	case RP1_SECONDARY:
		a = (a >> 8) & 31;
		if (a < 8 || a > 19)
			*frequency = 0;
		else
			*frequency = (*frequency + a / 2) / a;
		break;
	case RP1_HALF:
		*frequency = (*frequency + 1) / 2;
		break;
	case RP1_LEAF:
		b = rp1_clock_read(sc, def->divider) & def->maximum;
		if (b == 0)
			*frequency = 0;
		else
			*frequency /= b;
		break;
	}
 out:
	mtx_unlock(&sc->lock);
	return (error);
}

static int
rp1_clknode_mux(struct clknode *clk, int index)
{
	struct rp1_clknode_softc *node = clknode_get_softc(clk);
	struct rp1_clock_softc *sc = node->provider;
	const struct rp1_clock_def *def = node->def;
	uint32_t ctrl;
	int error;

	if (def->kind != RP1_LEAF || index < 0 || index >= def->nparents)
		return (EINVAL);
	mtx_lock(&sc->lock);
	if (sc->poisoned) {
		mtx_unlock(&sc->lock);
		return (EIO);
	}
	ctrl = rp1_clock_read(sc, def->reg) & ~(def->id == 12 ? 3U : 1U);
	if (def->auxiliary) {
		ctrl &= ~RP1_CLOCK_AUX;
		ctrl |= (def->mux[index] << 5) | 1;
	} else
		ctrl |= def->mux[index];
	rp1_clock_write(sc, def->reg, ctrl);
	/* Auxiliary selection uses the AUX SRC bit, not its AUXSRC index. */
	if ((ctrl & RP1_CLOCK_ENABLE) != 0 || def->id == 12 || def->id == 13)
		error = rp1_clock_wait(sc, def->selected, UINT32_MAX,
		    1U << (def->auxiliary ? 1 : def->mux[index]));
	else
		error = rp1_clock_parent(sc, def) == index ? 0 : EIO;
	mtx_unlock(&sc->lock);
	return (error);
}

static int
rp1_clknode_set_freq(struct clknode *clk, uint64_t fin, uint64_t *fout,
    int flags, int *stop)
{
	struct rp1_clknode_softc *node = clknode_get_softc(clk);
	struct rp1_clock_softc *sc = node->provider;
	const struct rp1_clock_def *def = node->def;
	struct rp1_clock_rate rate = { 0 }, candidate;
	uint64_t parent, requested = *fout;
	uint32_t reg, current, expected;
	int error, best = -1, index = clknode_get_parent_idx(clk);

	*stop = 1;
	if (sc->poisoned)
		return (EIO);
	if (def->ceiling != 0 && requested > def->ceiling)
		return (ERANGE);
	if (def->kind == RP1_LEAF) {
		/* Prefer the current parent on ties, and keep the graph in sync. */
		for (unsigned pass = 0; pass <= def->nparents; pass++) {
			unsigned i = pass == 0 ? (unsigned)index : pass - 1;
			if (i >= def->nparents || (pass != 0 && i == (unsigned)index))
				continue;
			struct clknode *p = clknode_find_by_name(clknode_get_parent_names(clk)[i]);
			if (p == NULL || clknode_get_freq(p, &parent) != 0 ||
			    rp1_clock_rate_plan(def->kind, parent, requested, flags,
			    def->maximum, &candidate) != 0)
				continue;
			if (best < 0 || rp1_clock_distance(candidate.actual, requested) <
			    rp1_clock_distance(rate.actual, requested)) {
				rate = candidate;
				best = i;
			}
		}
		if (best < 0)
			return (ERANGE);
	} else {
		error = rp1_clock_rate_plan(def->kind, fin, requested, flags, 0, &rate);
		if (error != 0)
			return (error);
	}
	*fout = rate.actual;
	if ((flags & CLK_SET_DRYRUN) != 0 || def->kind == RP1_HALF)
		return (0);
	mtx_lock(&sc->lock);
	current = rp1_clock_read(sc, def->reg);
	switch (def->kind) {
	case RP1_CORE:
		/* Match the BSD feedback programming sequence; poll the PLL lock. */
		if ((current & 0x3f) > 1) {
			error = EOPNOTSUPP;
			break;
		}
		reg = rp1_clock_read(sc, def->reg + 4);
		if ((current & 0x3f) == 1 && (reg & 0x3b) == 0 &&
		    (reg & RP1_PLL_DSMPD) == (rate.extra == 0 ? RP1_PLL_DSMPD : 0) &&
		    rp1_clock_read(sc, def->reg + 8) == rate.value &&
		    rp1_clock_read(sc, def->reg + 12) == rate.extra) {
			error = rp1_clock_wait(sc, def->reg, RP1_PLL_LOCK, RP1_PLL_LOCK);
			break;
		}
		reg &= ~(0x3bU | RP1_PLL_DSMPD);
		if (rate.extra == 0)
			reg |= RP1_PLL_DSMPD;
		/*
		 * Keep this write batch contiguous.  Reading back while feedback
		 * is zero could wait on a bus whose clock is being reprogrammed.
		 * Flush the ordered PCIe writes only after feedback is restored.
		 */
		bus_write_4(sc->mem, def->reg + 8, 0);
		bus_write_4(sc->mem, def->reg + 12, 0);
		bus_write_4(sc->mem, def->reg + 4, reg);
		bus_write_4(sc->mem, def->reg + 8, rate.value);
		bus_write_4(sc->mem, def->reg + 12, rate.extra);
		rp1_clock_write(sc, def->reg, current | 1);
		error = rp1_clock_wait(sc, def->reg, RP1_PLL_LOCK, RP1_PLL_LOCK);
		break;
	case RP1_PRIMARY:
		expected = (current & ~0x77000U) | (rate.value << 16) | (rate.extra << 12);
		rp1_clock_write(sc, def->reg, expected);
		error = (rp1_clock_read(sc, def->reg) & 0x77000) == (expected & 0x77000) ? 0 : EIO;
		break;
	case RP1_SECONDARY:
		expected = (current & ~0x1f00U) | (rate.value << 8);
		rp1_clock_write(sc, def->reg, expected | RP1_PLL_SECONDARY_RESET);
		DELAY(1); /* At least ten parent cycles for the supported VCO range. */
		rp1_clock_write(sc, def->reg, expected & ~RP1_PLL_SECONDARY_RESET);
		error = (rp1_clock_read(sc, def->reg) & 0x1f00) == (expected & 0x1f00) ? 0 : EIO;
		break;
	case RP1_LEAF:
		reg = rp1_clock_read(sc, def->divider);
		expected = (reg & ~def->maximum) | rate.value;
		rp1_clock_write(sc, def->divider, expected);
		error = (rp1_clock_read(sc, def->divider) & def->maximum) == rate.value ? 0 : EIO;
		break;
	default:
		error = EINVAL;
	}
	if (error != 0 && error != EOPNOTSUPP) {
		sc->poisoned = true;
		sc->ready = false;
	}
	mtx_unlock(&sc->lock);
	if (error == 0 && best >= 0)
		error = best == index ? rp1_clknode_mux(clk, best) :
		    clknode_set_parent_by_idx(clk, best);
	return (error);
}

static int
rp1_clknode_gate(struct clknode *clk, bool enable)
{
	struct rp1_clknode_softc *node = clknode_get_softc(clk);
	struct rp1_clock_softc *sc = node->provider;
	const struct rp1_clock_def *def = node->def;
	uint32_t value;
	int error = 0;

	mtx_lock(&sc->lock);
	if (sc->poisoned) {
		error = EIO;
		goto out;
	}
	if (def->kind == RP1_LEAF && def->id != 12 && def->id != 13) {
		value = rp1_clock_read(sc, def->reg);
		value = enable ? value | RP1_CLOCK_ENABLE : value & ~RP1_CLOCK_ENABLE;
		rp1_clock_write(sc, def->reg, value);
		error = (rp1_clock_read(sc, def->reg) & RP1_CLOCK_ENABLE) ==
		    (enable ? RP1_CLOCK_ENABLE : 0) ? 0 : EIO;
		if (error == 0 && enable)
			error = rp1_clock_wait(sc, def->selected, UINT32_MAX,
			    1U << (def->auxiliary ? 1 : def->mux[clknode_get_parent_idx(clk)]));
	} else if (def->kind == RP1_HALF && enable) {
		value = rp1_clock_read(sc, def->reg);
		rp1_clock_write(sc, def->reg, value | (1U << 4));
	} else if (def->kind == RP1_CORE && enable)
		error = rp1_clock_wait(sc, def->reg, RP1_PLL_LOCK, RP1_PLL_LOCK);
 out:
	if (error != 0) {
		sc->poisoned = true;
		sc->ready = false;
	}
	mtx_unlock(&sc->lock);
	return (error);
}

static clknode_method_t rp1_clknode_methods[] = {
	CLKNODEMETHOD(clknode_init, rp1_clknode_init),
	CLKNODEMETHOD(clknode_recalc_freq, rp1_clknode_recalc),
	CLKNODEMETHOD(clknode_set_freq, rp1_clknode_set_freq),
	CLKNODEMETHOD(clknode_set_mux, rp1_clknode_mux),
	CLKNODEMETHOD(clknode_set_gate, rp1_clknode_gate),
	CLKNODEMETHOD_END
};
DEFINE_CLASS_1(rp1_clknode, rp1_clknode_class, rp1_clknode_methods,
    sizeof(struct rp1_clknode_softc), clknode_class);

static int
rp1_clock_mapper(struct clkdom *domain, uint32_t ncells, phandle_t *cells,
    struct clknode **clk)
{
	struct rp1_clknode_softc *node;

	if (ncells != 1 || rp1_clock_index(cells[0]) < 0)
		return (EINVAL);
	*clk = clknode_find_by_id(domain, cells[0]);
	if (*clk == NULL)
		return (ENXIO);
	node = clknode_get_softc(*clk);
	return (node->provider->ready && !node->provider->poisoned ? 0 : ENXIO);
}

static int
rp1_clock_assignments(device_t dev)
{
	struct rp1_clock_softc *sc = device_get_softc(dev);
	phandle_t node = ofw_bus_get_node(dev);
	pcell_t clocks[RP1_CLOCK_ASSIGNMENTS * 2];
	int length, n;

	/* Parent choices are owned by the driver for this firmware profile. */
	if (OF_hasprop(node, "assigned-clock-parents"))
		return (EOPNOTSUPP);
	length = OF_getproplen(node, "assigned-clocks");
	if (length <= 0 || length > sizeof(clocks) || length % (2 * sizeof(*clocks)) != 0 ||
	    OF_getencprop(node, "assigned-clocks", clocks, sizeof(clocks)) != length)
		return (EINVAL);
	n = length / (2 * sizeof(*clocks));
	if (OF_getproplen(node, "assigned-clock-rates") != n * sizeof(*sc->rates) ||
	    OF_getencprop(node, "assigned-clock-rates", sc->rates, sizeof(sc->rates)) != n * sizeof(*sc->rates))
		return (EINVAL);
	for (int i = 0; i < n; i++) {
		if (OF_node_from_xref(clocks[2 * i]) != node ||
		    rp1_clock_index(clocks[2 * i + 1]) < 0 || sc->rates[i] == 0)
			return (EINVAL);
		sc->assigned[i] = clocks[2 * i + 1];
		for (int j = 0; j < i; j++)
			if (sc->assigned[j] == sc->assigned[i])
				return (EINVAL);
	}
	sc->nassigned = n;
	return (0);
}

/* Caller holds the clock topology lock. */
static int
rp1_clock_verify(struct rp1_clock_softc *sc)
{
	uint64_t frequency, tolerance;
	int j, error;

	tolerance = (sc->xosc_frequency + 0xffffff) / (1U << 24);
	for (unsigned i = 0; i < sc->nassigned; i++) {
		j = rp1_clock_index(sc->assigned[i]);
		error = clknode_get_freq(sc->nodes[j], &frequency);
		if (error != 0 || frequency == 0 ||
		    rp1_clock_distance(frequency, sc->rates[i]) > tolerance)
			return (ERANGE);
	}
	return (0);
}

static int
rp1_clock_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "raspberrypi,rp1-clocks"))
		return (ENXIO);
	device_set_desc(dev, "RP1 SYS/AUDIO and Ethernet clocks");
	return (BUS_PROBE_DEFAULT);
}

static int
rp1_clock_release(device_t dev)
{
	struct rp1_clock_softc *sc = device_get_softc(dev);
	int error;

	/* clkdom/clknode have no unregister API, including partial domains. */
	if (sc->domain != NULL)
		return (EBUSY);
	if (sc->mem != NULL) {
		error = bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem);
		if (error != 0)
			return (error);
		sc->mem = NULL;
	}
	if (sc->xosc != NULL) {
		error = clk_release(sc->xosc);
		if (error != 0)
			return (error);
		sc->xosc = NULL;
	}
	if (sc->lock_initialized) {
		mtx_destroy(&sc->lock);
		sc->lock_initialized = false;
	}
	return (0);
}

static int
rp1_clock_attach(device_t dev)
{
	struct rp1_clock_softc *sc = device_get_softc(dev);
	struct clknode_init_def init;
	struct rp1_clknode_softc *node;
	uint64_t frequency;
	pcell_t cells;
	int i, j, parent, rid = 0, error;

	if (OF_getencprop(ofw_bus_get_node(dev), "#clock-cells", &cells,
	    sizeof(cells)) != sizeof(cells) || cells != 1)
		return (EINVAL);
	error = rp1_clock_assignments(dev);
	if (error != 0)
		return (error);
	error = clk_get_by_ofw_index(dev, 0, 0, &sc->xosc);
	if (error != 0)
		goto fail;
	error = clk_get_freq(sc->xosc, &frequency);
	if (error != 0 || frequency == 0 || frequency > 100000000) {
		error = ERANGE;
		goto fail;
	}
	sc->xosc_frequency = frequency;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL || rman_get_size(sc->mem) < 0xc018) {
		error = ENXIO;
		goto fail;
	}
	mtx_init(&sc->lock, device_get_nameunit(dev), NULL, MTX_DEF);
	sc->lock_initialized = true;
	for (i = 0; i < RP1_CLOCK_COUNT; i++)
		snprintf(sc->names[i], sizeof(sc->names[i]), "%s_%d", device_get_nameunit(dev), rp1_clocks[i].id);
	sc->domain = clkdom_create(dev);
	clkdom_set_ofw_mapper(sc->domain, rp1_clock_mapper);
	for (i = 0; i < RP1_CLOCK_COUNT; i++) {
		memset(&init, 0, sizeof(init));
		init.id = rp1_clocks[i].id;
		init.name = sc->names[i];
		init.parent_cnt = rp1_clocks[i].nparents;
		for (j = 0; j < init.parent_cnt; j++) {
			parent = rp1_clock_index(rp1_clocks[i].parents[j]);
			sc->parents[i][j] = parent < 0 ? clk_get_name(sc->xosc) : sc->names[parent];
		}
		init.parent_names = sc->parents[i];
		if (rp1_clocks[i].kind != RP1_LEAF || rp1_clocks[i].id == 12 || rp1_clocks[i].id == 13)
			init.flags = CLK_NODE_CANNOT_STOP;
		sc->nodes[i] = clknode_create(sc->domain, &rp1_clknode_class, &init);
		if (sc->nodes[i] == NULL) {
			error = ENOMEM;
			goto fail;
		}
		node = clknode_get_softc(sc->nodes[i]);
		node->provider = sc;
		node->def = &rp1_clocks[i];
		if (clknode_register(sc->domain, sc->nodes[i]) == NULL) {
			error = ENXIO;
			goto fail;
		}
	}
	error = clkdom_finit(sc->domain);
	if (error != 0)
		goto fail;
	/* Unlike clk_set_assigned(), propagate every assignment failure. */
	clkdom_xlock(sc->domain);
	for (i = 0; i < sc->nassigned; i++) {
		j = rp1_clock_index(sc->assigned[i]);
		error = clknode_set_freq(sc->nodes[j], sc->rates[i], CLK_SET_ROUND_ANY, 0);
		if (error != 0)
			break;
	}
	if (error == 0)
		error = rp1_clock_verify(sc);
	clkdom_unlock(sc->domain);
	if (error != 0)
		goto fail;
	sc->ready = true;
	return (0);
fail:
	sc->ready = false;
	device_printf(dev, "clock initialization failed: %d\n", error);
	if (rp1_clock_release(dev) != 0) {
		device_printf(dev, "retaining unavailable clock provider and its resources\n");
		return (0);
	}
	return (error);
}

static device_method_t rp1_clock_methods[] = {
	DEVMETHOD(device_probe, rp1_clock_probe),
	DEVMETHOD(device_attach, rp1_clock_attach),
	DEVMETHOD(device_detach, rp1_clock_release),
	DEVMETHOD_END
};
static driver_t rp1_clock_driver = {
	"rp1_clock", rp1_clock_methods, sizeof(struct rp1_clock_softc)
};
EARLY_DRIVER_MODULE(rp1_clock, rp1, rp1_clock_driver, NULL, NULL,
    BUS_PASS_RESOURCE + BUS_PASS_ORDER_EARLY);
MODULE_VERSION(rp1_clock, 1);
