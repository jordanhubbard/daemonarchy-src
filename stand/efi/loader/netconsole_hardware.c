/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

#include <stand.h>
#include <efi.h>
#include <efilib.h>
#include <efipciio.h>
#include <netinet/in.h>
#include <netif.h>
#include <Protocol/SimpleNetwork.h>
#include <Protocol/NetworkInterfaceIdentifier.h>

#include "netconsole_undi.h"

static EFI_GUID sn_guid = EFI_SIMPLE_NETWORK_PROTOCOL_GUID;
static EFI_GUID nii_guid = EFI_NETWORK_INTERFACE_IDENTIFIER_PROTOCOL_GUID_31;
static EFI_GUID pci_guid = EFI_PCI_IO_PROTOCOL_GUID;

static void *
ancestor_protocol(EFI_HANDLE device, EFI_GUID *guid)
{
	EFI_DEVICE_PATH *path;
	EFI_HANDLE handle;
	void *protocol;

	/* SNP and NII may be installed on different handles on one PCI path. */
	if (BS->HandleProtocol(device, guid, &protocol) == EFI_SUCCESS)
		return (protocol);
	path = efi_lookup_devpath(device);
	if (path == NULL ||
	    BS->LocateDevicePath(guid, &path, &handle) != EFI_SUCCESS ||
	    BS->HandleProtocol(handle, guid, &protocol) != EFI_SUCCESS)
		return (NULL);
	return (protocol);
}

static int
readable(uint64_t address, uint64_t size,
    const struct nc_undi_region *regions, size_t count)
{
	const struct nc_undi_region *r;
	size_t i;

	if (size == 0 || address > UINT64_MAX - size)
		return (0);
	for (i = 0; i < count; i++) {
		r = &regions[i];
		/* Inspect only firmware/loader code or data, never arbitrary MMIO. */
		if (r->type < 1 || r->type > 6 ||
		    (r->attributes & UINT64_C(0x2000)) != 0)
			continue;
		if (address >= r->start && address - r->start < r->size &&
		    size <= r->size - (address - r->start))
			return (1);
	}
	return (0);
}

static void
inspect_undi(EFI_NETWORK_INTERFACE_IDENTIFIER_PROTOCOL *nii,
    const struct nc_undi_region *regions, size_t count)
{
	struct nc_undi_info info;
	enum nc_undi_status status;
	const uint8_t *rom;
	size_t length;

	printf("  UNDI %u.%u interface %u: ", nii->MajorVer,
	    nii->MinorVer, nii->IfNum);
	if (nii->Type != EfiNetworkInterfaceUndi || nii->Id == 0 ||
	    nii->Id > UINTPTR_MAX || !readable(nii->Id, 32, regions, count)) {
		printf("no readable software UNDI descriptor\n");
		return;
	}
	rom = (const void *)(uintptr_t)nii->Id;
	length = rom[4];
	if (!readable(nii->Id, length, regions, count)) {
		printf("descriptor extends outside mapped firmware memory\n");
		return;
	}
	status = nc_undi_inspect(rom, length, nii->Id, nii->IfNum,
	    regions, count, &info);
	printf("%s\n", nc_undi_status_string(status));
}

void
netconsole_hardware(void)
{
	EFI_SIMPLE_NETWORK *sn;
	EFI_NETWORK_INTERFACE_IDENTIFIER_PROTOCOL *nii;
	EFI_PCI_IO_PROTOCOL *pci;
	EFI_MEMORY_DESCRIPTOR *map, *md;
	struct nc_undi_region *regions;
	EFI_HANDLE handle;
	EFI_STATUS status;
	UINTN size, capacity, key, stride, segment, bus, device, function;
	UINT32 version, id;
	size_t i, count;
	unsigned unit;

	if (efinetif.netif_nifs == 0) {
		printf("No firmware-visible Ethernet interfaces.\n");
		return;
	}
	/* Allocate before taking the snapshot; never retain pointers into it. */
	size = 0;
	stride = sizeof(*map);
	status = BS->GetMemoryMap(&size, NULL, &key, &stride, &version);
	if (status != EFI_BUFFER_TOO_SMALL || stride < sizeof(*map) ||
	    size > 1024 * 1024 || stride > 4096) {
		printf("Cannot inspect firmware memory map.\n");
		return;
	}
	capacity = size + 8 * stride;
	map = malloc(capacity);
	regions = calloc(capacity / sizeof(*map), sizeof(*regions));
	if (map == NULL || regions == NULL) {
		free(map);
		free(regions);
		printf("Cannot allocate firmware inventory.\n");
		return;
	}
	size = capacity;
	status = BS->GetMemoryMap(&size, map, &key, &stride, &version);
	if (status != EFI_SUCCESS || stride < sizeof(*map) ||
	    size > capacity || size % stride != 0) {
		printf("Firmware memory snapshot changed; retry netdebug devices.\n");
		goto done;
	}
	count = size / stride;
	for (i = 0; i < count; i++) {
		md = (void *)((char *)map + i * stride);
		regions[i].start = md->PhysicalStart;
		if (md->NumberOfPages <= UINT64_MAX / EFI_PAGE_SIZE)
			regions[i].size = md->NumberOfPages * EFI_PAGE_SIZE;
		regions[i].type = md->Type;
		regions[i].attributes = md->Attribute;
	}
	for (unit = 0; unit < (unsigned)efinetif.netif_nifs; unit++) {
		handle = efi_find_handle(&efinet_dev, unit);
		printf("net%u: firmware Ethernet interface", unit);
		if (handle == NULL) {
			printf("; handle unavailable\n");
			continue;
		}
		if (BS->HandleProtocol(handle, &sn_guid, (void **)&sn) == EFI_SUCCESS &&
		    sn->Mode != NULL && sn->Mode->HwAddressSize == 6) {
			printf(" MAC %02x:%02x:%02x:%02x:%02x:%02x",
			    sn->Mode->CurrentAddress.Addr[0], sn->Mode->CurrentAddress.Addr[1],
			    sn->Mode->CurrentAddress.Addr[2], sn->Mode->CurrentAddress.Addr[3],
			    sn->Mode->CurrentAddress.Addr[4], sn->Mode->CurrentAddress.Addr[5]);
		}
		pci = ancestor_protocol(handle, &pci_guid);
		if (pci != NULL && pci->GetLocation(pci, &segment, &bus,
		    &device, &function) == EFI_SUCCESS &&
		    pci->Pci.Read(pci, EfiPciIoWidthUint32, 0, 1, &id) == EFI_SUCCESS)
			printf(" PCI %04lx:%02lx:%02lx.%lx %04x:%04x",
			    (unsigned long)segment, (unsigned long)bus,
			    (unsigned long)device, (unsigned long)function,
			    id & 0xffff, id >> 16);
		else
			printf("; PCI identity unavailable");
		printf("\n");
		nii = ancestor_protocol(handle, &nii_guid);
		if (nii == NULL)
			printf("  No UNDI 3.1 interface; early native backend required.\n");
		else
			inspect_undi(nii, regions, count);
	}
	printf("Inventory only: no NIC start, DMA change or kernel connection attempted.\n");
done:
	free(regions);
	free(map);
}
