/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */
/*
 * Kernel glue for the early polled network console.  Explicitly enabled by
 * netdebug.plain=1 and netdebug.early=1 plus the identity the EFI loader
 * exports (netdebug.pci, netdebug.pci_id, netdebug.hwaddr, local/peer
 * addresses and boot ID).  It owns the selected PCI function from early
 * startup until SI_SUB_ROOT_CONF, holds it against ordinary driver attach
 * with a placeholder newbus driver, then quiesces it and lets the native
 * driver attach.  See tools/daemonarchy/earlynet-ownership.md.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/cons.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/proc.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/atomic.h>
#include <machine/cpufunc.h>
#include <machine/pci_cfgreg.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <netinet/in.h>

#include <dev/daemonarchy/earlynet/earlynet.h>

#if !defined(__amd64__)
#error "earlynet supports amd64 PCI only"
#endif

#define	EN_MMIO_LEN	0x20000
/* Legacy virtio queue PFNs are 32 bits: keep DMA below 16 TiB. */
#define	EN_DMA_HIGH	((vm_paddr_t)((1ULL << 44) - 1))

static MALLOC_DEFINE(M_EARLYNET, "earlynet", "early debug transport rings");

static struct earlynet en;
static struct {
	int		domain, bus, slot, func;
	bool		io;
	uint16_t	port;
	void		*va;
} loc;
static device_t hold_dev;

/* ---- platform: register, DMA and timing access for the backends ---- */

static uint32_t
en_rd32(void *ctx __unused, uint32_t off)
{
	return (loc.io ? inl(loc.port + off) :
	    *(volatile uint32_t *)((char *)loc.va + off));
}

static void
en_wr32(void *ctx __unused, uint32_t off, uint32_t v)
{
	if (loc.io)
		outl(loc.port + off, v);
	else
		*(volatile uint32_t *)((char *)loc.va + off) = v;
}

static uint16_t
en_rd16(void *ctx __unused, uint32_t off)
{
	return (inw(loc.port + off));
}

static void
en_wr16(void *ctx __unused, uint32_t off, uint16_t v)
{
	outw(loc.port + off, v);
}

static uint8_t
en_rd8(void *ctx __unused, uint32_t off)
{
	return (inb(loc.port + off));
}

static void
en_wr8(void *ctx __unused, uint32_t off, uint8_t v)
{
	outb(loc.port + off, v);
}

static int
en_dma_alloc(void *ctx __unused, size_t size, size_t align,
    struct earlynet_dma *d)
{
	d->va = contigmalloc(size, M_EARLYNET, M_NOWAIT | M_ZERO, 0,
	    EN_DMA_HIGH, align, 0);
	if (d->va == NULL)
		return (ENOMEM);
	d->pa = vtophys(d->va);
	d->size = size;
	return (0);
}

static void
en_dma_free(void *ctx __unused, struct earlynet_dma *d)
{
	contigfree(d->va, d->size, M_EARLYNET);
	d->va = NULL;
}

static void
en_delay(void *ctx __unused, unsigned us)
{
	DELAY(us);
}

static void
en_sync(void *ctx __unused)
{
	atomic_thread_fence_seq_cst();
}

static const struct earlynet_platform en_platform = {
	.rd32 = en_rd32, .wr32 = en_wr32, .rd16 = en_rd16, .wr16 = en_wr16,
	.rd8 = en_rd8, .wr8 = en_wr8, .dma_alloc = en_dma_alloc,
	.dma_free = en_dma_free, .delay_us = en_delay, .sync = en_sync,
};

/* ---- console ---- */

static void en_cn_probe(struct consdev *cp) { cp->cn_pri = CN_DEAD; }
static void en_cn_init(struct consdev *cp __unused) {}
static void en_cn_term(struct consdev *cp __unused) {}
static void en_cn_grab(struct consdev *cp __unused) {}
static void en_cn_ungrab(struct consdev *cp __unused) {}

static int
en_cn_getc(struct consdev *cp __unused)
{
	return (earlynet_getc(&en));
}

static void
en_cn_putc(struct consdev *cp __unused, int c)
{
	earlynet_putc(&en, c);
	/*
	 * Send completed lines while booting, so silence after a reset still
	 * leaves the last stage with the controller.  Never transmit from a
	 * critical section or with a spin lock held; getc flushes those.
	 */
	if (c == '\n' && curthread->td_critnest == 0)
		(void)earlynet_flush_line(&en);
}

static const struct consdev_ops en_cn_ops = {
	.cn_probe = en_cn_probe, .cn_init = en_cn_init, .cn_term = en_cn_term,
	.cn_getc = en_cn_getc, .cn_putc = en_cn_putc, .cn_grab = en_cn_grab,
	.cn_ungrab = en_cn_ungrab,
};
static struct consdev en_console = {
	.cn_ops = &en_cn_ops,
	.cn_pri = CN_REMOTE,
	.cn_name = "daemonarchy_earlynet",
};

/* ---- start ---- */

static bool
env_is(const char *name, const char *value)
{
	char buf[8];

	return (getenv_string(name, buf, sizeof(buf)) != 0 &&
	    strcmp(buf, value) == 0);
}

/*
 * netdebug.early.<name> overrides netdebug.<name>.  It selects a NIC the
 * loader cannot use, such as an e1000 function with no firmware driver,
 * while the loader console runs on another NIC.
 */
static int
early_env(const char *name, char *buf, int size)
{
	char key[48];

	snprintf(key, sizeof(key), "netdebug.early.%s", name);
	if (getenv_string(key, buf, size) != 0)
		return (1);
	snprintf(key, sizeof(key), "netdebug.%s", name);
	return (getenv_string(key, buf, size));
}

static int
parse_mac(const char *text, uint8_t mac[6])
{
	unsigned i;
	char *end;

	for (i = 0; i < 6; i++) {
		unsigned long v = strtoul(text, &end, 16);

		if (end == text || v > 255 || (i < 5 ? *end != ':' : *end != 0))
			return (EINVAL);
		mac[i] = (uint8_t)v;
		text = end + 1;
	}
	return (0);
}

static int
locate(void)
{
	char text[64], *p, *end;
	unsigned long part[4];
	uint32_t id, bar;
	uint16_t vendor, device;
	int i;

	if (early_env("pci", text, sizeof(text)) == 0)
		return (ENOENT);
	for (i = 0, p = text; i < 4; i++) {
		part[i] = strtoul(p, &end, 10);
		if (end == p || (i < 3 ? *end != ':' : *end != 0))
			return (EINVAL);
		p = end + 1;
	}
	loc.domain = part[0];
	loc.bus = part[1];
	loc.slot = part[2];
	loc.func = part[3];
	if (pci_cfgregopen() == 0)
		return (ENXIO);
	id = pci_cfgregread(loc.domain, loc.bus, loc.slot, loc.func, PCIR_DEVVENDOR, 4);
	vendor = id & 0xffff;
	device = id >> 16;
	if (early_env("pci_id", text, sizeof(text)) == 0 ||
	    strtoul(text, &end, 16) != vendor || *end != ':' ||
	    strtoul(end + 1, NULL, 16) != device)
		return (ENODEV);	/* not the function the loader used */
	if (pci_cfgregread(loc.domain, loc.bus, loc.slot, loc.func, PCIR_CLASS, 1) !=
	    PCIC_NETWORK || (en.be = earlynet_backend_for(vendor, device)) == NULL)
		return (ENODEV);
	bar = pci_cfgregread(loc.domain, loc.bus, loc.slot, loc.func, PCIR_BAR(0), 4);
	if ((bar & PCIM_BAR_SPACE) == PCIM_BAR_IO_SPACE) {
		loc.io = true;
		loc.port = bar & PCIM_BAR_IO_BASE;
		if (loc.port == 0)
			return (ENXIO);
	} else {
		vm_paddr_t base = bar & PCIM_BAR_MEM_BASE;

		if ((bar & PCIM_BAR_MEM_TYPE) == PCIM_BAR_MEM_64)
			base |= (vm_paddr_t)pci_cfgregread(loc.domain, loc.bus,
			    loc.slot, loc.func, PCIR_BAR(1), 4) << 32;
		if (base == 0)
			return (ENXIO);
		loc.va = pmap_mapdev(base, EN_MMIO_LEN);
	}
	pci_cfgregwrite(loc.domain, loc.bus, loc.slot, loc.func, PCIR_COMMAND,
	    pci_cfgregread(loc.domain, loc.bus, loc.slot, loc.func, PCIR_COMMAND, 2) |
	    PCIM_CMD_BUSMASTEREN | (loc.io ? PCIM_CMD_PORTEN : PCIM_CMD_MEMEN), 2);
	return (0);
}

static void
unmap(void)
{
	if (loc.va != NULL) {
		pmap_unmapdev(loc.va, EN_MMIO_LEN);
		loc.va = NULL;
	}
}

static void
earlynet_sysinit(void *arg __unused)
{
	char text[64];
	uint8_t mac[6];
	struct in_addr local, peer;
	int error;

	if (!env_is("netdebug.plain", "1") || !env_is("netdebug.early", "1"))
		return;
	if (early_env("local_ip", text, sizeof(text)) == 0 ||
	    inet_aton(text, &local) == 0 ||
	    early_env("peer_ip", text, sizeof(text)) == 0 ||
	    inet_aton(text, &peer) == 0 ||
	    early_env("hwaddr", text, sizeof(text)) == 0 ||
	    parse_mac(text, mac) != 0 ||
	    getenv_string("netdebug.boot_id", en.boot_id, sizeof(en.boot_id)) == 0) {
		printf("earlynet: incomplete netdebug identity; not started\n");
		return;
	}
	if ((error = locate()) != 0) {
		printf("earlynet: no supported NIC at netdebug.pci (%d)\n", error);
		unmap();
		return;
	}
	en.pf = &en_platform;
	/* Expected address; an unprogrammed em function adopts it. */
	memcpy(en.mac, mac, sizeof(en.mac));
	en.local_ip = local.s_addr;
	en.peer_ip = peer.s_addr;
	error = earlynet_start(&en);
	if (error == 0 && memcmp(en.mac, mac, sizeof(mac)) != 0) {
		/* A different MAC is a different NIC than the loader used. */
		(void)earlynet_finish(&en);
		(void)earlynet_quiesce(&en);
		error = ENODEV;
	}
	if (error != 0) {
		printf("earlynet: %s backend not started (%d)%s\n", en.be->name,
		    error, en.owner == EARLYNET_FAILED ? "; NIC held" : "");
		if (en.owner == EARLYNET_RELEASED)
			unmap();
		return;
	}
	if (cnadd(&en_console) != 0) {
		(void)earlynet_finish(&en);
		if (earlynet_quiesce(&en) == 0)
			unmap();
		return;
	}
	printf("netdebug-stage: kernel-early boot_id=%s backend=%s pci=%d:%d:%d:%d\n",
	    en.boot_id, en.be->name, loc.domain, loc.bus, loc.slot, loc.func);
}
SYSINIT(daemonarchy_earlynet, SI_SUB_KMEM, SI_ORDER_ANY, earlynet_sysinit, NULL);

/*
 * Diagnostic only: netdebug.early_panic=1 panics on purpose during device
 * configuration, while earlynet still owns the NIC and before its native
 * driver can attach, to qualify the pre-driver panic path.
 */
static void
earlynet_deliberate_panic(void *arg __unused)
{
	if (en.owner == EARLYNET_OWNED && env_is("netdebug.early_panic", "1"))
		panic("netdebug: deliberate pre-driver panic (netdebug.early_panic=1)");
}
SYSINIT(daemonarchy_earlynet_panic, SI_SUB_CONFIGURE, SI_ORDER_ANY,
    earlynet_deliberate_panic, NULL);

/* ---- hold the device against ordinary attach until release ---- */

static int
hold_probe(device_t dev)
{
	if (earlynet_driver_may_attach(&en) || pci_get_domain(dev) != loc.domain ||
	    pci_get_bus(dev) != loc.bus || pci_get_slot(dev) != loc.slot ||
	    pci_get_function(dev) != loc.func)
		return (ENXIO);
	device_set_desc(dev, "Daemonarchy early debug transport (holding NIC)");
	return (BUS_PROBE_SPECIFIC);
}

static int
hold_attach(device_t dev)
{
	hold_dev = dev;
	return (0);
}

static int
hold_detach(device_t dev __unused)
{
	/* Never hand a NIC with possible DMA to another driver. */
	return (earlynet_driver_may_attach(&en) ? 0 : EBUSY);
}

static device_method_t hold_methods[] = {
	DEVMETHOD(device_probe, hold_probe),
	DEVMETHOD(device_attach, hold_attach),
	DEVMETHOD(device_detach, hold_detach),
	DEVMETHOD_END
};
static driver_t hold_driver = { "earlynet", hold_methods, 0 };
DRIVER_MODULE(daemonarchy_earlynet, pci, hold_driver, NULL, NULL);

static void
earlynet_release(void *arg __unused)
{
	device_t dev;

	if (en.owner != EARLYNET_OWNED)
		return;
	printf("netdebug-stage: kernel-early-release boot_id=%s\n", en.boot_id);
	/* No FINISHED: the mountroot console reheralds into the same session. */
	(void)earlynet_drain(&en);
	cnremove(&en_console);
	if (earlynet_quiesce(&en) != 0) {
		printf("earlynet: quiesce failed; the NIC stays held\n");
		return;
	}
	unmap();
	dev = hold_dev;
	if (dev == NULL)
		return;
	bus_topo_lock();
	if (device_detach(dev) == 0) {
		hold_dev = NULL;
		(void)device_probe_and_attach(dev);
	}
	bus_topo_unlock();
}
/* Before the diagnostic mountroot console (SI_ORDER_LAST) needs the NIC. */
SYSINIT(daemonarchy_earlynet_release, SI_SUB_ROOT_CONF, SI_ORDER_FIRST,
    earlynet_release, NULL);
