/* SPDX-License-Identifier: BSD-2-Clause */
/* Verify the actual FAT32 file extents before normal raw-slot writes.
 * Equal empty contents alone cannot prove which filename owns an extent. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "boottrace_fat.h"

#define BT_FAT_DIR_SECTORS 64
#define BT_FAT_EOC 0x0ffffff8U

static uint32_t
fat_u16(const unsigned char *p)
{

	return (p[0] | (uint32_t)p[1] << 8);
}

static uint32_t
fat_u32(const unsigned char *p)
{

	return (fat_u16(p) | fat_u16(p + 2) << 16);
}

static int
fat_entry(struct bt_fat *f, uint32_t cluster, uint32_t *value)
{
	uint64_t off, sector;
	uint32_t current = 0;
	unsigned copy;

	if (cluster > f->clusters + 1)
		return (-1);
	for (copy = 0; copy < f->fats; copy++) {
		off = f->fat_off + copy * f->fat_bytes + (uint64_t)cluster * 4;
		sector = off & ~(uint64_t)511;
		if (f->cache_off[copy] != sector) {
			if (sector > f->bytes - 512 ||
			    f->read(f->arg, sector, f->cache[copy], 512) != 0)
				return (-1);
			f->cache_off[copy] = sector;
		}
		*value = fat_u32(f->cache[copy] + (off - sector)) & 0x0fffffffU;
		if (copy != 0 && current != *value)
			return (-1);
		current = *value;
	}
	return (0);
}

int
bt_fat_open(struct bt_fat *f, bt_fat_read_fn readfn, void *arg, uint64_t bytes)
{
	unsigned char boot[512];
	uint32_t bps, spc, reserved, fat_sectors, total, zero, one;
	uint64_t clusters;

	if (f == NULL || readfn == NULL || bytes < 512 ||
	    readfn(arg, 0, boot, sizeof(boot)) != 0)
		return (-1);
	memset(f, 0, sizeof(*f));
	bps = fat_u16(boot + 11);
	spc = boot[13];
	reserved = fat_u16(boot + 14);
	fat_sectors = fat_u32(boot + 36);
	total = fat_u32(boot + 32);
	f->fats = boot[16];
	if (boot[510] != 0x55 || boot[511] != 0xaa ||
	    bps < 512 || bps > 4096 || (bps & (bps - 1)) != 0 ||
	    spc == 0 || spc > 128 || (spc & (spc - 1)) != 0 ||
	    reserved == 0 || fat_sectors == 0 || total == 0 ||
	    f->fats == 0 || f->fats > 2 || fat_u16(boot + 17) != 0 ||
	    fat_u16(boot + 22) != 0 || fat_u16(boot + 19) != 0 ||
	    fat_u16(boot + 42) != 0 || (fat_u16(boot + 40) & 0x80) != 0)
		return (-1);
	f->read = readfn;
	f->arg = arg;
	f->bytes = (uint64_t)total * bps;
	f->fat_off = (uint64_t)reserved * bps;
	f->fat_bytes = (uint64_t)fat_sectors * bps;
	f->data_off = f->fat_off + f->fats * f->fat_bytes;
	f->cluster_bytes = bps * spc;
	f->root = fat_u32(boot + 44);
	if (f->bytes > bytes || f->data_off >= f->bytes)
		return (-1);
	clusters = (f->bytes - f->data_off) / f->cluster_bytes;
	if (clusters < 65525 || clusters > 0x0fffffeeU ||
	    clusters + 2 > f->fat_bytes / 4 ||
	    f->root < 2 || f->root > clusters + 1)
		return (-1);
	f->clusters = clusters;
	f->cache_off[0] = f->cache_off[1] = UINT64_MAX;
	/* Require a clean filesystem with no recorded hard I/O error. A dirty
 * previous userspace mount needs recovery, not new raw writes. */
	if (fat_entry(f, 0, &zero) != 0 || fat_entry(f, 1, &one) != 0 ||
	    (zero & 0x0fffff00U) != 0x0fffff00U ||
	    (zero & 0xff) != boot[21] || one < BT_FAT_EOC)
		return (-1);
	return (0);
}

int
bt_fat_find(struct bt_fat *f, uint32_t directory, const char name[11],
    struct bt_fat_entry *out)
{
	unsigned char sector[512];
	struct bt_fat_entry found;
	uint64_t off;
	uint32_t cluster, next;
	unsigned reads = 0, pos, within;
	int matches = 0;

	if (f == NULL || name == NULL || out == NULL || f->read == NULL)
		return (-1);
	cluster = directory != 0 ? directory : f->root;
	for (;;) {
		if (cluster < 2 || cluster > f->clusters + 1)
			return (-1);
		if (fat_entry(f, cluster, &next) != 0 ||
		    (next < BT_FAT_EOC && (next < 2 || next > f->clusters + 1)))
			return (-1);
		off = f->data_off + (uint64_t)(cluster - 2) * f->cluster_bytes;
		for (within = 0; within < f->cluster_bytes; within += 512) {
			if (++reads > BT_FAT_DIR_SECTORS ||
			    off + within > f->bytes - 512 ||
			    f->read(f->arg, off + within, sector, sizeof(sector)) != 0)
				return (-1);
			for (pos = 0; pos < sizeof(sector); pos += 32) {
				if (sector[pos] == 0)
					goto done;
				/* Deleted names, long names and volume labels are not files. */
				if (sector[pos] == 0xe5 || (sector[pos + 11] & 8) != 0 ||
				    memcmp(sector + pos, name, 11) != 0)
					continue;
				if (++matches != 1 || (fat_u16(sector + pos + 20) & 0xf000) != 0)
					return (-1);
				found.attributes = sector[pos + 11];
				found.first = fat_u16(sector + pos + 26) |
				    fat_u16(sector + pos + 20) << 16;
				found.size = fat_u32(sector + pos + 28);
				if (found.first < 2 || found.first > f->clusters + 1)
					return (-1);
				found.off = f->data_off +
				    (uint64_t)(found.first - 2) * f->cluster_bytes;
			}
		}
		if (next >= BT_FAT_EOC)
			break;
		cluster = next;
	}
done:
	/* End-of-directory may precede unused allocated tail clusters. Still
 * verify that tail terminates instead of accepting a free/cyclic chain. */
	while (next < BT_FAT_EOC) {
		if (++reads > BT_FAT_DIR_SECTORS || next < 2 ||
		    next > f->clusters + 1 || fat_entry(f, next, &next) != 0)
			return (-1);
	}
	if (matches != 1)
		return (-1);
	*out = found;
	return (0);
}

int
bt_fat_file(struct bt_fat *f, uint32_t directory, const char name[11],
    struct bt_fat_entry *out)
{
	struct bt_fat_entry entry;
	uint32_t cluster, next, count, i;

	if (bt_fat_find(f, directory, name, &entry) != 0 ||
	    (entry.attributes & 0xd8) != 0 || entry.size == 0 || entry.size > 16384)
		return (-1);
	count = (entry.size + f->cluster_bytes - 1) / f->cluster_bytes;
	cluster = entry.first;
	for (i = 0; i < count; i++) {
		if (fat_entry(f, cluster, &next) != 0)
			return (-1);
		if (i + 1 == count) {
			if (next < BT_FAT_EOC)
				return (-1);
		} else if (next != cluster + 1 || next > f->clusters + 1)
			return (-1);
		cluster = next;
	}
	*out = entry;
	return (0);
}
