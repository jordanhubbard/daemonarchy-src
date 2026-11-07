/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "boottrace_acpi.h"

static uint8_t memory[8192];
static unsigned captures, rejects, reads;
static void put32(size_t off, uint32_t v) {
	unsigned i;
	for (i = 0; i < 4; i++) memory[off + i] = v >> (i * 8);
}
static void fix(size_t off, size_t len, size_t check) {
	uint8_t sum = 0;
	size_t i;
	memory[off + check] = 0;
	for (i = 0; i < len; i++) sum += memory[off + i];
	memory[off + check] = -sum;
}
static int read_memory(void *arg, uint64_t off, void *out, size_t n) {
	(void)arg;
	reads++;
	if (off > sizeof(memory) || n > sizeof(memory) - off) return (-1);
	memcpy(out, memory + off, n);
	return (0);
}
static void emit(void *arg, const char *name, const void *data, size_t n,
    const char *error) {
	(void)arg; (void)name;
	if (data != NULL) { assert(n > 0 && error == NULL); captures++; }
	else { assert(n == 0 && error != NULL); rejects++; }
}
static void setup(void) {
	memset(memory, 0, sizeof(memory));
	memcpy(memory + 64, "RSD PTR ", 8);
	memory[64 + 15] = 2;
	put32(64 + 20, 36); put32(64 + 24, 128);
	fix(64, 20, 8); fix(64, 36, 32);
	memcpy(memory + 128, "XSDT", 4); put32(132, 44); put32(164, 256);
	fix(128, 44, 9);
	memcpy(memory + 256, "APIC", 4); put32(260, 44); fix(256, 44, 9);
	captures = rejects = reads = 0;
}
int main(void) {
	setup();
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == 0);
	assert(captures == 3 && rejects == 6 && reads < 20);
	setup(); memory[64] ^= 1;
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == -1);
	assert(captures == 0);
	setup(); put32(132, UINT32_MAX);
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == -1);
	assert(captures == 1);
	setup(); put32(64 + 20, UINT32_MAX); fix(64, 36, 32);
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == -1);
	setup(); memory[300 - 1] ^= 1;
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == 0);
	assert(captures == 2 && rejects == 7);
	setup(); put32(164, UINT32_MAX); fix(128, 44, 9);
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == 0);
	assert(captures == 2 && rejects == 7);
	setup(); put32(132, 45); fix(128, 45, 9);
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == -1);
	setup(); put32(132, 36 + 257 * 8); fix(128, 36 + 257 * 8, 9);
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == -1);
	setup(); put32(64 + 16, 128); memory[64 + 15] = 0;
	memcpy(memory + 128, "RSDT", 4); put32(132, 40); put32(164, 256);
	fix(64, 20, 8); fix(128, 40, 9);
	assert(bt_acpi_snapshot(64, read_memory, emit, NULL) == 0);
	assert(captures == 3);
	assert(bt_acpi_snapshot(UINT64_MAX, read_memory, emit, NULL) == -1);
	puts("PASS: ACPI v1/v2, checksums, bounds, oversized and unmapped tables");
	return (0);
}
