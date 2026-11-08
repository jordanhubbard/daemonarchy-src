/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Cold, explicitly selected diagnostic profile for the owned BCM2711 QEMU
 * controller.  The production translated backend remains the implementation.
 * No live tag replacement, physical-device binding, or DMA success is faked.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/mutex.h>
#include <sys/sysctl.h>
#include <machine/bus.h>
#include <machine/bus_dma.h>
#include <arm64/broadcom/bcm2712_dma_policy.h>
#include <arm64/broadcom/bcm2711_dma_test.h>

static int enabled;
TUNABLE_INT("hw.bcm2711_dma_test.enable", &enabled);
static int ready;
static uint32_t window_low, window_high;
static bus_dma_tag_t translated_tag, physical_tag;
static struct mtx witness_mtx;
MTX_SYSINIT(bcm_dma_test_mtx, &witness_mtx, "DMA test witness", MTX_SPIN);
static uintptr_t witness_start;
static size_t witness_length;
static struct bcm2711_dma_test_witness witness;
static uint64_t coverage[4];
static u_int ring_mask, ring_errors;
static uint64_t dcbaap, erstba, crcr;
SYSCTL_NODE(_hw, OID_AUTO, bcm2711_dma_test, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Diagnostic simulator DMA profile; not physical acceptance");
SYSCTL_INT(_hw_bcm2711_dma_test, OID_AUTO, ready, CTLFLAG_RD, &ready, 0, "Paired root tag/window ready");
SYSCTL_UINT(_hw_bcm2711_dma_test, OID_AUTO, window_low, CTLFLAG_RD, &window_low, 0, "BAR2 low readback");
SYSCTL_UINT(_hw_bcm2711_dma_test, OID_AUTO, window_high, CTLFLAG_RD, &window_high, 0, "BAR2 high readback");

SYSCTL_UINT(_hw_bcm2711_dma_test, OID_AUTO, ring_mask, CTLFLAG_RD, &ring_mask, 0, "Actual xHCI programmed rings");
SYSCTL_UINT(_hw_bcm2711_dma_test, OID_AUTO, ring_errors, CTLFLAG_RD, &ring_errors, 0, "Invalid translated ring addresses");
SYSCTL_U64(_hw_bcm2711_dma_test, OID_AUTO, dcbaap, CTLFLAG_RD, &dcbaap, 0, "Actual programmed DCBAAP");
SYSCTL_U64(_hw_bcm2711_dma_test, OID_AUTO, erstba, CTLFLAG_RD, &erstba, 0, "Actual programmed ERSTBA");
SYSCTL_U64(_hw_bcm2711_dma_test, OID_AUTO, crcr, CTLFLAG_RD, &crcr, 0, "Actual programmed CRCR");

int
bcm2711_dma_test_requested(void)
{
	return (enabled == 1);
}

int
bcm2711_dma_test_create(bus_dma_tag_t parent, bus_dma_tag_t *out)
{
	struct bcm2712_dma_policy policy;
	const struct bcm2712_dma_root_window window = {
		.cpu_base = 0, .parent_bus_base = BCM2711_DMA_TEST_OFFSET,
		.length = BCM2711_DMA_TEST_OFFSET, .role = BCM2712_DMA_RAM,
		.space_flags = 0x43000000
	};
	int error;

	*out = NULL;
	if (!bcm2711_dma_test_requested() || ready)
		return (ENXIO);
	error = bcm2712_dma_policy_root(&policy, &window, 1);
	if (error != 0)
		return (error);
	/* The forthcoming opt-in constructor must be present after backend merge.
	 * Ordinary USB descendants use ordinary bus_dma_tag_create inheritance.
	 */
	return (bus_dma_tag_create_bcm2712_ram(parent, &policy, 1, 0,
	    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, 0x3c000000, 65535,
	    0x3c000000, 0, NULL, NULL, out));
}

int
bcm2711_dma_test_publish(device_t dev, bus_dma_tag_t tag,
    bus_dma_tag_t physical, uint32_t low, uint32_t high)
{
	if (ready || tag == NULL || physical == NULL ||
	    low != BCM2711_DMA_TEST_WINDOW_LOW ||
	    high != BCM2711_DMA_TEST_WINDOW_HIGH)
		return (EINVAL);
	/* Both tags are borrowed from the attached host's retained tag chain.
	 * The host has no dynamic detach path; this diagnostic context lives to
	 * guest reset.  Never destroy either through the observer/module.
	 */
	translated_tag = tag;
	physical_tag = physical;
	window_low = low;
	window_high = high;
	ready = 1;
	device_printf(dev, "diagnostic DMA paired: PCI %#jx -> CPU0, BAR2 %x:%x\n",
	    (uintmax_t)BCM2711_DMA_TEST_OFFSET, high, low);
	return (0);
}

int
bcm2711_dma_test_tags(bus_dma_tag_t *translated, bus_dma_tag_t *physical)
{
	if (!ready)
		return (ENXIO);
	*translated = translated_tag;
	*physical = physical_tag;
	return (0);
}

int
bcm2711_dma_test_begin(void *buffer, size_t length)
{
	int error;

	if (!ready || buffer == NULL || length == 0 || length > 131072 ||
	    ((uintptr_t)buffer & 511) != 0 || (length & 511) != 0 ||
	    (uintptr_t)buffer > UINTPTR_MAX - length)
		return (EINVAL);
	mtx_lock_spin(&witness_mtx);
	error = witness_length != 0 ? EBUSY : 0;
	if (error == 0) {
		bzero(&witness, sizeof(witness));
		bzero(coverage, sizeof(coverage));
		witness_start = (uintptr_t)buffer;
		witness_length = length;
	}
	mtx_unlock_spin(&witness_mtx);
	return (error);
}

void
bcm2711_dma_test_end(struct bcm2711_dma_test_witness *out)
{
	mtx_lock_spin(&witness_mtx);
	*out = witness;
	witness_length = 0;
	witness_start = 0;
	mtx_unlock_spin(&witness_mtx);
}

void
bcm2711_dma_test_usb_load(void *buffer, size_t length,
    bus_dma_segment_t *segments, int count, int error)
{
	uintptr_t address;
	uint64_t total;
	int i;
	size_t first, sector;

	address = (uintptr_t)buffer;
	mtx_lock_spin(&witness_mtx);
	if (witness_length == 0 || address < witness_start ||
	    address - witness_start >= witness_length) {
		mtx_unlock_spin(&witness_mtx);
		return;
	}
	witness.callbacks++;
	if (length == 0 || length > witness_length - (address - witness_start) ||
	    error != 0 || segments == NULL || count <= 0 ||
	    ((address - witness_start) & 511) != 0 || (length & 511) != 0) {
		witness.errors++;
		mtx_unlock_spin(&witness_mtx);
		return;
	}
	total = 0;
	for (i = 0; i < count; i++) {
		/* Parent physical prefix is deliberately <=960MiB. Actual emitted
		 * addresses, not a recomputed policy vector, witness translation.
		 */
		if (segments[i].ds_addr < BCM2711_DMA_TEST_OFFSET ||
		    segments[i].ds_len == 0 ||
		    segments[i].ds_addr - BCM2711_DMA_TEST_OFFSET >= 0x3c001000 ||
		    segments[i].ds_len > 0x3c001000 -
		    (segments[i].ds_addr - BCM2711_DMA_TEST_OFFSET))
			witness.errors++;
		if (witness.first_device == 0)
			witness.first_device = segments[i].ds_addr;
		witness.last_device = segments[i].ds_addr;
		total += segments[i].ds_len;
	}
	if (total != length)
		witness.errors++;
	witness.bytes += length;
	first = (address - witness_start) / 512;
	for (sector = first; sector < first + length / 512; sector++) {
		if ((coverage[sector / 64] & (UINT64_C(1) << (sector % 64))) == 0) {
			coverage[sector / 64] |= UINT64_C(1) << (sector % 64);
			witness.covered_bytes += 512;
		}
	}
	mtx_unlock_spin(&witness_mtx);
}

void
bcm2711_dma_test_ring(const char *name, uint64_t address)
{
	u_int bit;

	if (!ready)
		return;
	bit = strcmp(name, "DCBAAP") == 0 ? 1 :
	    strcmp(name, "ERSTBA") == 0 ? 2 : 4;
	if (address < BCM2711_DMA_TEST_OFFSET ||
	    address - BCM2711_DMA_TEST_OFFSET >= 0x3c001000)
		ring_errors++;
	if (bit == 1) dcbaap = address;
	if (bit == 2) erstba = address;
	if (bit == 4) crcr = address;
	if ((ring_mask & bit) == 0)
		printf("BCM_DMA_TEST ring %s=%#jx\n", name, (uintmax_t)address);
	ring_mask |= bit;
}
