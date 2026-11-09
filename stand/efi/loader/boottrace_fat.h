/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _BOOTTRACE_FAT_H_
#define _BOOTTRACE_FAT_H_
#include <stddef.h>
#include <stdint.h>

/* Fresh, bounded reads relative to the selected partition, never writes. */
typedef int (*bt_fat_read_fn)(void *, uint64_t, void *, size_t);
struct bt_fat {
	bt_fat_read_fn read;
	void *arg;
	uint64_t bytes, fat_off, fat_bytes, data_off;
	uint32_t root, clusters, cluster_bytes;
	unsigned fats;
	uint64_t cache_off[2];
	unsigned char cache[2][512];
};
struct bt_fat_entry {
	uint64_t off;
	uint32_t first, size;
	unsigned char attributes;
};

int bt_fat_open(struct bt_fat *, bt_fat_read_fn, void *, uint64_t);
/* A zero directory selects the root. Names are exact eleven-byte 8.3 names. */
int bt_fat_find(struct bt_fat *, uint32_t, const char[11], struct bt_fat_entry *);
/* Require a regular file with its entire allocated cluster chain contiguous. */
int bt_fat_file(struct bt_fat *, uint32_t, const char[11], struct bt_fat_entry *);
#endif
