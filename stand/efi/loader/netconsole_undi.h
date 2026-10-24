/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */
#ifndef _NETCONSOLE_UNDI_H_
#define _NETCONSOLE_UNDI_H_

#include <stddef.h>
#include <stdint.h>

struct nc_undi_region {
	uint64_t start, size, attributes;
	uint32_t type;
};

struct nc_undi_info {
	uint64_t entry;
	uint32_t implementation;
	uint16_t interface;
};

enum nc_undi_status {
	NC_UNDI_CANDIDATE,
	NC_UNDI_FORMAT,
	NC_UNDI_CHECKSUM,
	NC_UNDI_VERSION,
	NC_UNDI_INTERFACE,
	NC_UNDI_HARDWARE,
	NC_UNDI_BUS,
	NC_UNDI_ADDRESS,
	NC_UNDI_TABLE_MEMORY,
	NC_UNDI_ENTRY_MEMORY
};

enum nc_undi_status nc_undi_inspect(const void *, size_t, uint64_t,
    uint16_t, const struct nc_undi_region *, size_t, struct nc_undi_info *);
const char *nc_undi_status_string(enum nc_undi_status);
void netconsole_hardware(void);

#endif
