/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

#include "netconsole_undi.h"

/* Layout/constants from UEFI Appendix E, retained Uefi/UefiPxe.h.
 * Byte reads keep packed, unaligned firmware tables portable in the fixture.
 */
static uint32_t
u32(const uint8_t *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static uint64_t
u64(const uint8_t *p)
{
	return ((uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32);
}

static int
runtime_range(uint64_t address, uint64_t size,
    const struct nc_undi_region *regions, size_t count, int code)
{
	const struct nc_undi_region *r;
	size_t i;

	if (size == 0 || address > UINT64_MAX - size)
		return (0);
	for (i = 0; i < count; i++) {
		r = &regions[i];
		/* EFI runtime attribute; runtime code=5, runtime data=6.
		 * Never promote boot-services memory to runtime by assumption.
		 */
		if ((r->attributes & UINT64_C(0x8000000000000000)) == 0 ||
		    (r->attributes & UINT64_C(0x2000)) != 0 ||
		    (r->type != 5 && (code || r->type != 6)) ||
		    (code && (r->attributes & UINT64_C(0x4000)) != 0))
			continue;
		if (r->size > UINT64_MAX - r->start)
			continue;
		if (address >= r->start && address - r->start < r->size &&
		    size <= r->size - (address - r->start))
			return (1);
	}
	return (0);
}

enum nc_undi_status
nc_undi_inspect(const void *data, size_t available, uint64_t address,
    uint16_t interface, const struct nc_undi_region *regions, size_t count,
    struct nc_undi_info *info)
{
	const uint8_t *p = data;
	uint64_t entry;
	uint32_t implementation;
	size_t i, length;
	uint8_t checksum;
	int pci;

	if (info == NULL)
		return (NC_UNDI_FORMAT);
	*info = (struct nc_undi_info){0};
	if (regions == NULL && count != 0)
		return (NC_UNDI_FORMAT);
	if (p == NULL || available < 32 || u32(p) != UINT32_C(0x45585021))
		return (NC_UNDI_FORMAT);
	length = p[4];
	if (length < 32 || length > available || p[27] == 0 ||
	    (size_t)p[27] > (length - 28) / 4 || p[11] != 0 ||
	    p[24] != 0 || p[25] != 0 || p[26] != 0)
		return (NC_UNDI_FORMAT);
	checksum = 0;
	for (i = 0; i < length; i++)
		checksum += p[i];
	if (checksum != 0)
		return (NC_UNDI_CHECKSUM);
	if (p[6] != 2 || p[8] != 3 || p[9] != 1)
		return (NC_UNDI_VERSION);
	if (interface > ((uint16_t)p[7] | (uint16_t)p[10] << 8))
		return (NC_UNDI_INTERFACE);
	implementation = u32(p + 12);
	if ((implementation & UINT32_C(0x80000000)) != 0)
		return (NC_UNDI_HARDWARE);
	pci = 0;
	for (i = 0; i < p[27]; i++)
		if (u32(p + 28 + 4 * i) == UINT32_C(0x52494350))
			pci = 1;
	if (!pci)
		return (NC_UNDI_BUS);
	entry = u64(p + 16);
	if ((implementation & UINT32_C(0x40000000)) == 0) {
		if (entry > UINT64_MAX - address)
			return (NC_UNDI_ADDRESS);
		entry += address;
	}
	if (entry == 0)
		return (NC_UNDI_ADDRESS);
	if (!runtime_range(address, length, regions, count, 0))
		return (NC_UNDI_TABLE_MEMORY);
	if (!runtime_range(entry, 1, regions, count, 1))
		return (NC_UNDI_ENTRY_MEMORY);
	info->entry = entry;
	info->implementation = implementation;
	info->interface = interface;
	return (NC_UNDI_CANDIDATE);
}

const char *
nc_undi_status_string(enum nc_undi_status status)
{
	switch (status) {
	case NC_UNDI_CANDIDATE: return ("runtime descriptor candidate; kernel transport not qualified");
	case NC_UNDI_FORMAT: return ("invalid UNDI descriptor");
	case NC_UNDI_CHECKSUM: return ("UNDI descriptor checksum mismatch");
	case NC_UNDI_VERSION: return ("unsupported UNDI version");
	case NC_UNDI_INTERFACE: return ("UNDI interface out of range");
	case NC_UNDI_HARDWARE: return ("hardware UNDI not supported");
	case NC_UNDI_BUS: return ("UNDI does not advertise PCI");
	case NC_UNDI_ADDRESS: return ("invalid UNDI entry address");
	case NC_UNDI_TABLE_MEMORY: return ("UNDI descriptor is not in runtime memory");
	case NC_UNDI_ENTRY_MEMORY: return ("UNDI entry is not executable runtime code");
	}
	return ("unknown UNDI inspection result");
}
