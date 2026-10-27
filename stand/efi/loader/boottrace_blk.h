/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

/*
 * Raw-block writes for the hardware-trace stick.  The image builder
 * (build-hwtrace-image.sh, stick.py write-map) records where each
 * preallocated, contiguous file of the DATRACE volume lives in
 * SLOTS.MAP; the loader then rewrites those sectors through
 * EFI_BLOCK_IO, flushes the partition and its disk, and reads the sectors
 * back through the disk's BlockIo, never trusting the firmware FAT
 * driver's cache.  Include EFI and BlockIo declarations before this header.
 */

#ifndef _BOOTTRACE_BLK_H_
#define	_BOOTTRACE_BLK_H_

#define	BT_MAP_TAG	"Daemonarchy DATRACE block map v1"
#define	BT_MAP_MAX	256
#define	BT_MAP_NAME	24
#define	BT_MAP_FILEMAX	16384	/* largest mapped file */
#define	BT_BLK_ALIGN	4096	/* largest IoAlign / block size handled */
#define	BT_BLK_BUF	(BT_MAP_FILEMAX + 2 * BT_BLK_ALIGN)

struct bt_map_ent {
	char		name[BT_MAP_NAME];	/* "STATE.TXT", "TRACES/0001.TXT" */
	uint64_t	off;		/* byte offset in the partition */
	uint32_t	size;		/* file size in bytes */
};

struct bt_map {
	uint64_t	part_lba;	/* partition start, 512-byte sectors */
	uint64_t	part_sectors;	/* partition length, 512-byte sectors */
	uint64_t	data_off;	/* first data cluster, bytes */
	unsigned	count;
	struct bt_map_ent ent[BT_MAP_MAX];
};

struct bt_blk {
	EFI_BLOCK_IO_PROTOCOL *part;	/* the DATRACE partition */
	EFI_BLOCK_IO_PROTOCOL *disk;	/* its whole disk, or NULL */
	uint64_t	disk_lba0;	/* partition start in disk blocks */
	uint32_t	media_id;	/* partition MediaId when checked */
	int		drop;		/* test: skip writes and verification */
	int		verify;		/* DBT_LV_* of the last write */
	int		via_disk;	/* the last verify read used the disk */
};

uint32_t bt_fnv1a(const void *, size_t);
int	bt_map_parse(const char *, size_t, struct bt_map *, char *, size_t);
const struct bt_map_ent *bt_map_find(const struct bt_map *, const char *);
EFI_STATUS bt_blk_check(struct bt_blk *, const struct bt_map *, char *,
	    size_t);
EFI_STATUS bt_blk_read(struct bt_blk *, const struct bt_map_ent *, char *,
	    size_t);
EFI_STATUS bt_blk_write(struct bt_blk *, const struct bt_map_ent *,
	    const char *, size_t);

#endif
