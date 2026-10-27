/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

/*
 * Block map parsing and raw sector rewrites for the hardware-trace stick;
 * see boottrace_blk.h.  The map (DATRACE/SLOTS.MAP) is text:
 *
 *   Daemonarchy DATRACE block map v1
 *   part_lba=264192
 *   part_sectors=524288
 *   data_off=4210688
 *   file=STATE.TXT off=4214784 size=512
 *   file=TRACES/0001.TXT off=4231168 size=16384
 *   ...
 *   sum=89abcdef
 *
 * Offsets are bytes from the start of the partition; part_lba and
 * part_sectors are 512-byte sectors.  sum is FNV-1a (32 bit, hex) of every
 * byte before the "sum=" line.  Trailing newline padding is ignored.
 */

#ifndef BT_BLK_FIXTURE
#include <stand.h>
#include <efi.h>
#include <efilib.h>
#include <Protocol/BlockIo.h>
#include <sys/daemonarchy_boottrace.h>
#endif
#include "boottrace_blk.h"

static uint8_t bt_blkbuf[BT_BLK_BUF] __attribute__((aligned(BT_BLK_ALIGN)));
static uint8_t bt_blkver[BT_BLK_BUF] __attribute__((aligned(BT_BLK_ALIGN)));

uint32_t
bt_fnv1a(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint32_t h = 2166136261u;

	while (len-- > 0) {
		h ^= *p++;
		h *= 16777619u;
	}
	return (h);
}

/* Parse "key=" followed by a decimal (or, for sum, hex) number. */
static int
bt_num(const char *p, const char *end, const char *key, int base,
    uint64_t *out)
{
	size_t klen = strlen(key);
	uint64_t v = 0;
	int digits = 0, d;

	if ((size_t)(end - p) <= klen || strncmp(p, key, klen) != 0)
		return (-1);
	for (p += klen; p < end && *p != ' ' && *p != '\n'; p++) {
		if (*p >= '0' && *p <= '9')
			d = *p - '0';
		else if (base == 16 && *p >= 'a' && *p <= 'f')
			d = *p - 'a' + 10;
		else
			return (-1);
		if (v > (UINT64_MAX - d) / base)
			return (-1);
		v = v * base + d;
		digits++;
	}
	if (digits == 0 || digits > 20)
		return (-1);
	*out = v;
	return (0);
}

static int
bt_why(char *why, size_t whylen, const char *msg, unsigned line)
{
	if (why != NULL && whylen > 0)
		snprintf(why, whylen, "%s (line %u)", msg, line);
	return (-1);
}

int
bt_map_parse(const char *text, size_t len, struct bt_map *m, char *why,
    size_t whylen)
{
	const char *p = text, *end = text + len, *eol, *q;
	uint64_t v, off, size, sum;
	unsigned line = 1, i;
	int have = 0;
	struct bt_map_ent *e;

	memset(m, 0, sizeof(*m));
	while (end > text && (end[-1] == '\n' || end[-1] == '\0'))
		end--;
	eol = memchr(p, '\n', end - p);
	if (eol == NULL || (size_t)(eol - p) != sizeof(BT_MAP_TAG) - 1 ||
	    strncmp(p, BT_MAP_TAG, eol - p) != 0)
		return (bt_why(why, whylen, "not a v1 block map", line));
	for (p = eol + 1; p < end; p = eol + 1) {
		line++;
		eol = memchr(p, '\n', end - p);
		if (eol == NULL)
			eol = end;
		if (bt_num(p, eol, "sum=", 16, &sum) == 0) {
			if (eol != end)
				return (bt_why(why, whylen,
				    "text after the sum line", line));
			if (sum != bt_fnv1a(text, p - text))
				return (bt_why(why, whylen, "checksum mismatch",
				    line));
			if (have != 7 || m->count == 0)
				return (bt_why(why, whylen, "missing fields",
				    line));
			return (0);
		}
		if (bt_num(p, eol, "part_lba=", 10, &v) == 0) {
			m->part_lba = v;
			have |= 1;
		} else if (bt_num(p, eol, "part_sectors=", 10, &v) == 0) {
			m->part_sectors = v;
			have |= 2;
		} else if (bt_num(p, eol, "data_off=", 10, &v) == 0) {
			m->data_off = v;
			have |= 4;
		} else if (eol - p > 5 && strncmp(p, "file=", 5) == 0) {
			if (have != 7)
				return (bt_why(why, whylen,
				    "file before the partition fields", line));
			if (m->count >= BT_MAP_MAX)
				return (bt_why(why, whylen, "too many files",
				    line));
			p += 5;
			q = memchr(p, ' ', eol - p);
			if (q == NULL || q == p ||
			    (size_t)(q - p) >= BT_MAP_NAME)
				return (bt_why(why, whylen, "bad file name",
				    line));
			e = &m->ent[m->count];
			memcpy(e->name, p, q - p);
			e->name[q - p] = '\0';
			for (i = 0; e->name[i] != '\0'; i++)
				if (e->name[i] <= ' ' || e->name[i] > '~' ||
				    e->name[i] == '\\')
					return (bt_why(why, whylen,
					    "bad file name", line));
			p = q + 1;
			q = memchr(p, ' ', eol - p);
			if (q == NULL || bt_num(p, q, "off=", 10, &off) != 0 ||
			    bt_num(q + 1, eol, "size=", 10, &size) != 0)
				return (bt_why(why, whylen, "bad file extent",
				    line));
			/* Sector aligned, inside the data area, small. */
			if (off % 512 != 0 || off < m->data_off || size == 0 ||
			    size > BT_MAP_FILEMAX ||
			    off + size > m->part_sectors * 512)
				return (bt_why(why, whylen,
				    "extent outside the data area", line));
			for (i = 0; i < m->count; i++)
				if (off < m->ent[i].off + m->ent[i].size &&
				    m->ent[i].off < off + size)
					return (bt_why(why, whylen,
					    "overlapping extents", line));
			e->off = off;
			e->size = size;
			m->count++;
		} else
			return (bt_why(why, whylen, "unknown line", line));
	}
	return (bt_why(why, whylen, "no sum line", line));
}

/* Look up "\STATE.TXT", "\TRACES\0001.TXT" or "TRACES/0001.TXT". */
const struct bt_map_ent *
bt_map_find(const struct bt_map *m, const char *path)
{
	unsigned i, j;
	char a, b;

	while (*path == '\\' || *path == '/')
		path++;
	for (i = 0; i < m->count; i++) {
		for (j = 0;; j++) {
			a = path[j] == '\\' ? '/' : path[j];
			b = m->ent[i].name[j];
			if (a >= 'a' && a <= 'z')
				a -= 'a' - 'A';
			if (b >= 'a' && b <= 'z')
				b -= 'a' - 'A';
			if (a != b)
				break;
			if (a == '\0')
				return (&m->ent[i]);
		}
	}
	return (NULL);
}

/*
 * The map must describe this very partition: same length, and, when the
 * device path names the partition's start, the same start.
 */
EFI_STATUS
bt_blk_check(struct bt_blk *b, const struct bt_map *m, char *why,
    size_t whylen)
{
	EFI_BLOCK_IO_MEDIA *media;
	uint64_t bytes;

	if (b->part == NULL || (media = b->part->Media) == NULL) {
		snprintf(why, whylen, "no BlockIo on the data volume");
		return (EFI_UNSUPPORTED);
	}
	if (!media->MediaPresent || media->ReadOnly) {
		snprintf(why, whylen, "medium absent or read-only");
		return (EFI_WRITE_PROTECTED);
	}
	if (media->BlockSize < 512 || media->BlockSize > BT_BLK_ALIGN ||
	    (media->BlockSize & (media->BlockSize - 1)) != 0 ||
	    media->IoAlign > BT_BLK_ALIGN) {
		snprintf(why, whylen, "block size %u / IoAlign %u unsupported",
		    (unsigned)media->BlockSize, (unsigned)media->IoAlign);
		return (EFI_UNSUPPORTED);
	}
	bytes = (media->LastBlock + 1) * media->BlockSize;
	if (bytes != m->part_sectors * 512) {
		snprintf(why, whylen, "partition is %ju bytes, map says %ju",
		    (uintmax_t)bytes, (uintmax_t)m->part_sectors * 512);
		return (EFI_VOLUME_CORRUPTED);
	}
	if (b->disk != NULL) {
		EFI_BLOCK_IO_MEDIA *dm = b->disk->Media;

		if (dm == NULL || dm->BlockSize != media->BlockSize ||
		    b->disk_lba0 * dm->BlockSize != m->part_lba * 512 ||
		    dm->IoAlign > BT_BLK_ALIGN) {
			snprintf(why, whylen, "partition starts at block %ju "
			    "of the disk, map says sector %ju",
			    (uintmax_t)b->disk_lba0, (uintmax_t)m->part_lba);
			return (EFI_VOLUME_CORRUPTED);
		}
	}
	b->media_id = media->MediaId;
	why[0] = '\0';
	return (EFI_SUCCESS);
}

/* Block range covering an extent; 0 when it does not fit the buffer. */
static size_t
bt_blk_span(struct bt_blk *b, const struct bt_map_ent *e, uint64_t *lba,
    size_t *head)
{
	uint32_t bs = b->part->Media->BlockSize;
	uint64_t first = e->off / bs, last = (e->off + e->size + bs - 1) / bs;

	*lba = first;
	*head = e->off % bs;
	if ((last - first) * bs > BT_BLK_BUF)
		return (0);
	return ((size_t)(last - first) * bs);
}

/*
 * Fresh read of the extent's blocks: through the whole disk's BlockIo when
 * known (no partition or DiskIo layer in between), else the partition's.
 */
static EFI_STATUS
bt_blk_fresh(struct bt_blk *b, uint64_t lba, size_t bytes, void *buf)
{
	if (b->part->Media->MediaId != b->media_id)
		return (EFI_MEDIA_CHANGED);
	if (b->disk != NULL) {
		b->via_disk = 1;
		return (b->disk->ReadBlocks(b->disk, b->disk->Media->MediaId,
		    b->disk_lba0 + lba, bytes, buf));
	}
	b->via_disk = 0;
	return (b->part->ReadBlocks(b->part, b->media_id, lba, bytes, buf));
}

EFI_STATUS
bt_blk_read(struct bt_blk *b, const struct bt_map_ent *e, char *out,
    size_t len)
{
	uint64_t lba;
	size_t bytes, head;
	EFI_STATUS status;

	bytes = bt_blk_span(b, e, &lba, &head);
	if (bytes == 0)
		return (EFI_BAD_BUFFER_SIZE);
	status = bt_blk_fresh(b, lba, bytes, bt_blkver);
	if (status != EFI_SUCCESS)
		return (status);
	memcpy(out, bt_blkver + head, MIN(len, e->size));
	return (EFI_SUCCESS);
}

/*
 * Rewrite a mapped file: data, then newline padding to its full size.
 * Partial blocks at either end are read first.  Success means the blocks
 * were written, flushed on the partition and the disk, and read back equal.
 */
EFI_STATUS
bt_blk_write(struct bt_blk *b, const struct bt_map_ent *e, const char *data,
    size_t len)
{
	EFI_BLOCK_IO_PROTOCOL *p = b->part;
	uint32_t bs = p->Media->BlockSize;
	uint64_t lba;
	size_t bytes, head;
	EFI_STATUS status, fl;

	b->verify = DBT_LV_NONE;
	bytes = bt_blk_span(b, e, &lba, &head);
	if (bytes == 0)
		return (EFI_BAD_BUFFER_SIZE);
	if (len > e->size)
		len = e->size;		/* keep the head of an oversized report */
	if (p->Media->MediaId != b->media_id || !p->Media->MediaPresent)
		return (EFI_MEDIA_CHANGED);
	if (head != 0 || (e->off + e->size) % bs != 0) {
		status = p->ReadBlocks(p, b->media_id, lba, bytes, bt_blkbuf);
		if (status != EFI_SUCCESS)
			return (status);
	}
	memcpy(bt_blkbuf + head, data, len);
	memset(bt_blkbuf + head + len, '\n', e->size - len);
	if (b->drop) {
		/* Test: behave like firmware that acknowledges and loses. */
		b->verify = DBT_LV_SKIPPED;
		return (EFI_SUCCESS);
	}
	status = p->WriteBlocks(p, b->media_id, lba, bytes, bt_blkbuf);
	if (status != EFI_SUCCESS)
		return (status);
	status = p->FlushBlocks(p);
	if (b->disk != NULL) {
		fl = b->disk->FlushBlocks(b->disk);
		if (status == EFI_SUCCESS)
			status = fl;
	}
	if (status != EFI_SUCCESS)
		return (status);
	memset(bt_blkver, 0, bytes);
	status = bt_blk_fresh(b, lba, bytes, bt_blkver);
	if (status != EFI_SUCCESS) {
		b->verify = DBT_LV_ERROR;
		return (status);
	}
	if (memcmp(bt_blkver + head, bt_blkbuf + head, e->size) != 0) {
		b->verify = DBT_LV_MISMATCH;
		return (EFI_DEVICE_ERROR);
	}
	b->verify = DBT_LV_OK;
	return (EFI_SUCCESS);
}
