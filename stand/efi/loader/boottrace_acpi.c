/* SPDX-License-Identifier: BSD-2-Clause */
/* Bounded, read-only ACPI discovery; no AML evaluation or physical probing. */
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
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

	if (address == 0 || read(arg, address, out, 36) != 0)
		return ("header outside readable firmware memory");
	*length = u32(out + 4);
	if (*length < 36 || *length > BT_ACPI_MAX)
		return ("table length outside capture limit");
	if (read(arg, address, out, *length) != 0)
		return ("table outside readable firmware memory");
	if (!checksum(out, *length))
		return ("checksum mismatch");
	return (NULL);
}

int
bt_acpi_snapshot(uint64_t address, bt_acpi_read_fn read,
    bt_acpi_emit_fn emit, void *arg)
{
	static const char *names[] = { "APIC", "PPTT", "FACP", "IORT",
	    "SRAT", "SLIT", "SPCR" };
	const char *why, *signature;
	uint64_t next;
	size_t n, rootlen, width, i, j, count;
	unsigned seen = 0;

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
		if (next == 0 || read(arg, next, table, 36) != 0)
			continue;
		for (j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
			if (memcmp(table, names[j], 4) != 0)
				continue;
			if ((seen & (1u << j)) != 0) {
				emit(arg, names[j], NULL, 0, "duplicate root entry");
				break;
			}
			seen |= 1u << j;
			why = load(next, table, &n, read, arg);
			emit(arg, names[j], why == NULL ? table : NULL,
			    why == NULL ? n : 0, why);
			break;
		}
	}
	for (j = 0; j < sizeof(names) / sizeof(names[0]); j++)
		if ((seen & (1u << j)) == 0)
			emit(arg, names[j], NULL, 0, "not present");
	return (0);
}
