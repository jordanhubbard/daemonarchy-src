/* SPDX-License-Identifier: BSD-2-Clause */
/* Bounded, read-only ACPI discovery; no AML evaluation or physical probing. */
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "boottrace_acpi.h"

static uint8_t root[BT_ACPI_MAX], table[BT_ACPI_MAX];

static uint32_t
u32(const uint8_t *p)
{

	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static uint64_t
u64(const uint8_t *p)
{

	return (u32(p) | (uint64_t)u32(p + 4) << 32);
}

static int
checksum(const uint8_t *p, size_t n)
{
	uint8_t sum = 0;

	while (n-- != 0)
		sum += *p++;
	return (sum == 0);
}

static const char *
load(uint64_t address, uint8_t *out, size_t *length,
    bt_acpi_read_fn read, void *arg)
{
	uint8_t signature[4];

	if (address == 0 || read(arg, address, out, 36) != 0)
		return ("header outside readable firmware memory");
	*length = u32(out + 4);
	memcpy(signature, out, sizeof(signature));
	if (*length < 36 || *length > BT_ACPI_MAX)
		return ("table length outside capture limit");
	if (read(arg, address, out, *length) != 0)
		return ("table outside readable firmware memory");
	if (u32(out + 4) != *length ||
	    memcmp(out, signature, sizeof(signature)) != 0)
		return ("table changed during capture");
	if (!checksum(out, *length))
		return ("checksum mismatch");
	return (NULL);
}

static int
snapshot(uint64_t address, bt_acpi_read_fn read,
    bt_acpi_emit_fn emit, void *arg, bool namespace)
{
	static const char *names[] = { "APIC", "PPTT", "FACP", "IORT",
	    "SRAT", "SLIT", "SPCR" };
	const char *why, *signature;
	uint64_t next, dsdt = 0;
	size_t n, rootlen, width, i, j, count;
	unsigned seen = 0;
	unsigned ssdts = 0;
	char ssname[] = "SS00";
	bool overflow = false;
	bool unreadable = false, ambiguous = false;

	if (address == 0 || read(arg, address, table, 20) != 0 ||
	    memcmp(table, "RSD PTR ", 8) != 0 || !checksum(table, 20)) {
		emit(arg, "RSDP", NULL, 0, "missing, unreadable or invalid RSDP");
		return (-1);
	}
	width = 4;
	n = 20;
	next = u32(table + 16);
	signature = "RSDT";
	if (table[15] >= 2) {
		if (read(arg, address, table, 36) != 0 ||
		    (n = u32(table + 20)) < 36 || n > 4096 ||
		    read(arg, address, table, n) != 0 || !checksum(table, n)) {
			emit(arg, "RSDP", NULL, 0, "invalid extended RSDP");
			return (-1);
		}
		if (u64(table + 24) != 0) {
			next = u64(table + 24);
			width = 8;
			signature = "XSDT";
		}
	}
	emit(arg, "RSDP", table, n, NULL);
	why = load(next, root, &rootlen, read, arg);
	if (why == NULL && (memcmp(root, signature, 4) != 0 ||
	    (rootlen - 36) % width != 0 ||
	    (rootlen - 36) / width > BT_ACPI_ENTRIES))
		why = "invalid root signature, entry count or alignment";
	if (why != NULL) {
		emit(arg, "ROOT", NULL, 0, why);
		return (-1);
	}
	emit(arg, "ROOT", root, rootlen, NULL);
	count = (rootlen - 36) / width;
	for (i = 0; i < count; i++) {
		next = width == 8 ? u64(root + 36 + i * width) :
		    u32(root + 36 + i * width);
		if (next == 0 || read(arg, next, table, 36) != 0) {
			unreadable = true;
			continue;
		}
		if (namespace && memcmp(table, "SSDT", 4) == 0) {
			if (ssdts == BT_ACPI_SSDTS) {
				overflow = true;
				continue;
			}
			ssname[3] = "0123456789ABCDEF"[ssdts++];
			why = load(next, table, &n, read, arg);
			if (why == NULL && memcmp(table, "SSDT", 4) != 0)
				why = "SSDT signature changed during capture";
			emit(arg, ssname, why == NULL ? table : NULL,
			    why == NULL ? n : 0, why);
			continue;
		}
		for (j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
			if (memcmp(table, names[j], 4) != 0)
				continue;
			if ((seen & (1u << j)) != 0) {
				if (strcmp(names[j], "FACP") == 0)
					ambiguous = true;
				emit(arg, names[j], NULL, 0, "duplicate root entry");
				break;
			}
			seen |= 1u << j;
			why = load(next, table, &n, read, arg);
			if (why == NULL && memcmp(table, names[j], 4) != 0)
				why = "table signature changed during capture";
			if (namespace && why == NULL &&
			    strcmp(names[j], "FACP") == 0) {
				/* FADT's X_DSDT supersedes its legacy 32-bit DSDT. */
				if (n >= 148)
					dsdt = u64(table + 140);
				if (dsdt == 0 && n >= 44)
					dsdt = u32(table + 40);
			}
			emit(arg, names[j], why == NULL ? table : NULL,
			    why == NULL ? n : 0, why);
			break;
		}
	}
	for (j = 0; j < sizeof(names) / sizeof(names[0]); j++)
		if ((seen & (1u << j)) == 0)
			emit(arg, names[j], NULL, 0, "not present");
	if (namespace) {
		why = dsdt == 0 ? "no DSDT address in validated FADT" :
		    load(dsdt, table, &n, read, arg);
		if (why == NULL && memcmp(table, "DSDT", 4) != 0)
			why = "invalid DSDT signature";
		emit(arg, "DSDT", why == NULL ? table : NULL,
		    why == NULL ? n : 0, why);
		for (; ssdts < BT_ACPI_SSDTS; ssdts++) {
			ssname[3] = "0123456789ABCDEF"[ssdts];
			emit(arg, ssname, NULL, 0, "not present");
		}
		emit(arg, "SSOV", NULL, 0, overflow ?
		    "SSDT capture limit exceeded; namespace incomplete" :
		    unreadable ? "unreadable root entry; namespace incomplete" :
		    ambiguous ? "ambiguous FADT; namespace incomplete" :
		    "not present");
	}
	return (0);
}

int
bt_acpi_snapshot(uint64_t address, bt_acpi_read_fn read,
    bt_acpi_emit_fn emit, void *arg)
{

	return (snapshot(address, read, emit, arg, false));
}

int
bt_acpi_namespace_snapshot(uint64_t address, bt_acpi_read_fn read,
    bt_acpi_emit_fn emit, void *arg)
{

	return (snapshot(address, read, emit, arg, true));
}
