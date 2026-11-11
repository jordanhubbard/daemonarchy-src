/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _AMD64_BIOS_BOOTMAP_H_
#define _AMD64_BIOS_BOOTMAP_H_

/* KERNBASE leaves a 2 GiB virtual window before the address space wraps. */
#define AMD64_BOOTMAP_LIMIT 0x80000000
#define AMD64_BOOTMAP_PT2_PAGES 2
#define AMD64_BOOTMAP_PT2_ENTRIES (512 * AMD64_BOOTMAP_PT2_PAGES)

#endif
