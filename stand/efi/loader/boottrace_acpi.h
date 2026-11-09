/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _BOOTTRACE_ACPI_H_
#define _BOOTTRACE_ACPI_H_

#define BT_ACPI_MAX 16384
#define BT_ACPI_ENTRIES 256
/* The reader must reject unmapped/device memory before dereferencing it. */
typedef int (*bt_acpi_read_fn)(void *, uint64_t, void *, size_t);
/* NULL data records a rejected/missing table, never a successful capture. */
typedef void (*bt_acpi_emit_fn)(void *, const char *, const void *, size_t,
    const char *);
int bt_acpi_snapshot(uint64_t, bt_acpi_read_fn, bt_acpi_emit_fn, void *);

#endif
