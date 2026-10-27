/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Hardware-trace stick block map and raw rewrites (stand/efi/loader/
 * boottrace_blk.c) against a fake EFI_BLOCK_IO disk and partition; not a
 * firmware or disk acceptance test.  The fake partition can stand in for
 * firmware that acknowledges writes, serves them back from a cache, and
 * never writes the media (the MSI Raider finding).
 */
#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t EFI_STATUS;
typedef uint64_t UINT64;
typedef uint64_t EFI_LBA;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef uint8_t BOOLEAN;
#define EFI_SUCCESS		0
#define EFI_UNSUPPORTED		3
#define EFI_BAD_BUFFER_SIZE	4
#define EFI_DEVICE_ERROR	7
#define EFI_WRITE_PROTECTED	8
#define EFI_VOLUME_CORRUPTED	10
#define EFI_MEDIA_CHANGED	13
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define DBT_LV_NONE		0
#define DBT_LV_OK		1
#define DBT_LV_MISMATCH		2
#define DBT_LV_ERROR		3
#define DBT_LV_SKIPPED		4

typedef struct {
	UINT32 MediaId;
	BOOLEAN RemovableMedia, MediaPresent, LogicalPartition, ReadOnly;
	BOOLEAN WriteCaching;
	UINT32 BlockSize, IoAlign;
	EFI_LBA LastBlock;
} EFI_BLOCK_IO_MEDIA;
typedef struct blkio EFI_BLOCK_IO_PROTOCOL;
struct blkio {
	EFI_BLOCK_IO_MEDIA *Media;
	EFI_STATUS (*ReadBlocks)(EFI_BLOCK_IO_PROTOCOL *, UINT32, EFI_LBA,
	    UINTN, void *);
	EFI_STATUS (*WriteBlocks)(EFI_BLOCK_IO_PROTOCOL *, UINT32, EFI_LBA,
	    UINTN, void *);
	EFI_STATUS (*FlushBlocks)(EFI_BLOCK_IO_PROTOCOL *);
	uint64_t base;		/* first block on the fake disk */
	int flushes;
};
#define BT_BLK_FIXTURE
#include "boottrace_blk.c"

#define DISK_BLOCKS	4096
#define PART_LBA	64	/* in 512-byte sectors */
static uint8_t disk[DISK_BLOCKS * 512];
static uint8_t cache[DISK_BLOCKS * 512];	/* firmware cache view */
static int lossy;		/* partition writes reach only the cache */
static int writes;
static EFI_BLOCK_IO_MEDIA pmedia, dmedia;
static EFI_BLOCK_IO_PROTOCOL part, whole;

static EFI_STATUS
fake_read(EFI_BLOCK_IO_PROTOCOL *b, UINT32 id, EFI_LBA lba, UINTN n, void *buf)
{
	uint64_t at = (b->base + lba) * b->Media->BlockSize;

	assert(n % b->Media->BlockSize == 0);
	assert(((uintptr_t)buf % (b->Media->IoAlign ? b->Media->IoAlign : 1)) == 0);
	if (id != b->Media->MediaId)
		return (EFI_MEDIA_CHANGED);
	assert(at + n <= sizeof(disk));
	memcpy(buf, (b == &part && lossy ? cache : disk) + at, n);
	return (EFI_SUCCESS);
}

static EFI_STATUS
fake_write(EFI_BLOCK_IO_PROTOCOL *b, UINT32 id, EFI_LBA lba, UINTN n, void *buf)
{
	uint64_t at = (b->base + lba) * b->Media->BlockSize;

	assert(n % b->Media->BlockSize == 0);
	if (id != b->Media->MediaId)
		return (EFI_MEDIA_CHANGED);
	assert(at + n <= sizeof(disk));
	memcpy(cache + at, buf, n);
	if (!lossy)
		memcpy(disk + at, buf, n);
	writes++;
	return (EFI_SUCCESS);
}

static EFI_STATUS
fake_flush(EFI_BLOCK_IO_PROTOCOL *b)
{
	b->flushes++;
	return (EFI_SUCCESS);
}

static void
setup(uint32_t bs, uint64_t part_bytes)
{
	memset(disk, '#', sizeof(disk));
	memcpy(cache, disk, sizeof(cache));
	lossy = writes = 0;
	dmedia = (EFI_BLOCK_IO_MEDIA){ .MediaId = 7, .MediaPresent = 1,
	    .BlockSize = bs, .IoAlign = 4, .LastBlock = sizeof(disk) / bs - 1 };
	pmedia = dmedia;
	pmedia.LogicalPartition = 1;
	pmedia.LastBlock = part_bytes / bs - 1;
	whole = (EFI_BLOCK_IO_PROTOCOL){ &dmedia, fake_read, fake_write,
	    fake_flush, 0, 0 };
	part = (EFI_BLOCK_IO_PROTOCOL){ &pmedia, fake_read, fake_write,
	    fake_flush, PART_LBA * 512 / bs, 0 };
}

/* A map with a valid sum; extra is inserted before the sum line. */
static size_t
make_map(char *out, size_t len, uint64_t part_sectors, const char *files)
{
	int n;

	n = snprintf(out, len, "%s\npart_lba=%d\npart_sectors=%" PRIu64
	    "\ndata_off=4096\n%s", BT_MAP_TAG, PART_LBA, part_sectors, files);
	n += snprintf(out + n, len - n, "sum=%08x\n", bt_fnv1a(out, n));
	/* The loader sees newline padding after the text. */
	memset(out + n, '\n', 64);
	return (n + 64);
}

static const char *files =
    "file=STATE.TXT off=8192 size=512\n"
    "file=TRACES/0001.TXT off=16384 size=16384\n"
    "file=TRACES/0002.TXT off=33280 size=1000\n";

static void
test_parse(void)
{
	struct bt_map m;
	char text[2048], why[96];
	size_t n;

	n = make_map(text, sizeof(text), 2048, files);
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) == 0);
	assert(m.count == 3 && m.part_lba == PART_LBA && m.part_sectors == 2048);
	assert(bt_map_find(&m, "\\TRACES\\0001.TXT") == &m.ent[1]);
	assert(bt_map_find(&m, "state.txt") == &m.ent[0]);
	assert(bt_map_find(&m, "\\TRACES\\0003.TXT") == NULL);
	assert(m.ent[1].off == 16384 && m.ent[1].size == 16384);

	text[60] ^= 1;			/* corrupted byte */
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) != 0);
	assert(strstr(why, "checksum") != NULL || strstr(why, "line") != NULL);

	n = make_map(text, sizeof(text), 2048,
	    "file=A off=8192 size=512\nfile=B off=8192 size=512\n");
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) != 0);
	assert(strstr(why, "overlapping") != NULL);
	n = make_map(text, sizeof(text), 2048, "file=A off=8193 size=512\n");
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) != 0);
	n = make_map(text, sizeof(text), 64, "file=A off=32768 size=512\n");
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) != 0);
	assert(strstr(why, "outside") != NULL);
	n = make_map(text, sizeof(text), 2048, "file=A off=1024 size=512\n");
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) != 0);
	n = make_map(text, sizeof(text), 2048, "file=A off=8192 size=99999\n");
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) != 0);
	n = make_map(text, sizeof(text), 2048, "file=A\\B off=8192 size=512\n");
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) != 0);
	n = make_map(text, sizeof(text), 2048, "");
	assert(bt_map_parse(text, n, &m, why, sizeof(why)) != 0);
	strcpy(text, "Daemonarchy DATRACE block map v2\n");
	assert(bt_map_parse(text, strlen(text), &m, why, sizeof(why)) != 0);
	puts("ok parse");
}

static void
load(struct bt_map *m, uint64_t part_sectors)
{
	char text[2048], why[96];
	size_t n = make_map(text, sizeof(text), part_sectors, files);

	assert(bt_map_parse(text, n, m, why, sizeof(why)) == 0);
}

static void
test_check(void)
{
	struct bt_map m;
	struct bt_blk b = { 0 };
	char why[96];

	setup(512, 2048 * 512);
	load(&m, 2048);
	b.part = &part;
	b.disk = &whole;
	b.disk_lba0 = PART_LBA;
	assert(bt_blk_check(&b, &m, why, sizeof(why)) == EFI_SUCCESS);
	b.disk_lba0 = PART_LBA + 1;	/* another partition */
	assert(bt_blk_check(&b, &m, why, sizeof(why)) == EFI_VOLUME_CORRUPTED);
	b.disk_lba0 = PART_LBA;
	load(&m, 2047);			/* map from another image */
	assert(bt_blk_check(&b, &m, why, sizeof(why)) == EFI_VOLUME_CORRUPTED);
	load(&m, 2048);
	pmedia.ReadOnly = 1;
	assert(bt_blk_check(&b, &m, why, sizeof(why)) == EFI_WRITE_PROTECTED);
	pmedia.ReadOnly = 0;
	pmedia.IoAlign = 8192;
	assert(bt_blk_check(&b, &m, why, sizeof(why)) == EFI_UNSUPPORTED);
	puts("ok check");
}

static void
test_write(uint32_t bs, int with_disk)
{
	struct bt_map m;
	struct bt_blk b = { 0 };
	char why[96], got[16384];
	const struct bt_map_ent *e;
	uint64_t at;

	setup(bs, 2048 * 512);
	load(&m, 2048);
	b.part = &part;
	if (with_disk) {
		b.disk = &whole;
		b.disk_lba0 = PART_LBA * 512 / bs;
	}
	assert(bt_blk_check(&b, &m, why, sizeof(why)) == EFI_SUCCESS);
	/* An unaligned, odd-sized extent: neighbours must survive. */
	e = bt_map_find(&m, "TRACES/0002.TXT");
	assert(bt_blk_write(&b, e, "hello\n", 6) == EFI_SUCCESS);
	assert(b.verify == DBT_LV_OK && b.via_disk == with_disk);
	at = PART_LBA * 512 + e->off;
	assert(memcmp(disk + at, "hello\n\n\n", 8) == 0);
	assert(disk[at + e->size - 1] == '\n' && disk[at + e->size] == '#');
	assert(disk[at - 1] == '#');
	assert(part.flushes == 1 && whole.flushes == with_disk);
	assert(bt_blk_read(&b, e, got, 6) == EFI_SUCCESS);
	assert(memcmp(got, "hello\n", 6) == 0);
	/* Oversized data keeps its head. */
	e = bt_map_find(&m, "STATE.TXT");
	memset(got, 'x', sizeof(got));
	assert(bt_blk_write(&b, e, got, sizeof(got)) == EFI_SUCCESS);
	at = PART_LBA * 512 + e->off;
	assert(disk[at + 511] == 'x' && disk[at + 512] == '#');
	printf("ok write bs=%u disk=%d\n", bs, with_disk);
}

static void
test_lost_writes(void)
{
	struct bt_map m;
	struct bt_blk b = { 0 };
	char why[96];
	const struct bt_map_ent *e;

	setup(512, 2048 * 512);
	load(&m, 2048);
	b.part = &part;
	b.disk = &whole;
	b.disk_lba0 = PART_LBA;
	assert(bt_blk_check(&b, &m, why, sizeof(why)) == EFI_SUCCESS);
	e = bt_map_find(&m, "STATE.TXT");
	/* The partition path acknowledges and caches; the disk never sees it. */
	lossy = 1;
	assert(bt_blk_write(&b, e, "next=2\n", 7) == EFI_DEVICE_ERROR);
	assert(b.verify == DBT_LV_MISMATCH);
	/* Test drop: nothing written, success reported, verify skipped. */
	lossy = 0;
	writes = 0;
	b.drop = 1;
	assert(bt_blk_write(&b, e, "next=3\n", 7) == EFI_SUCCESS);
	assert(writes == 0 && b.verify == DBT_LV_SKIPPED);
	b.drop = 0;
	/* A different medium in the slot. */
	pmedia.MediaId++;
	assert(bt_blk_write(&b, e, "next=4\n", 7) == EFI_MEDIA_CHANGED);
	puts("ok lost writes");
}

int
main(void)
{
	test_parse();
	test_check();
	test_write(512, 1);
	test_write(512, 0);
	test_write(4096, 1);
	test_lost_writes();
	puts("boottrace block map: all checks passed");
	return (0);
}
