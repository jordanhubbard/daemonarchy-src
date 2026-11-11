/* SPDX-License-Identifier: BSD-2-Clause */
/* Read-only EFI device-path validation for normal installation tracing. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "boottrace_media.h"

static uint32_t
bt_media_u32(const unsigned char *p)
{

	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
}

static uint64_t
bt_media_u64(const unsigned char *p)
{

	return (bt_media_u32(p) | (uint64_t)bt_media_u32(p + 4) << 32);
}

int
bt_media_parse(const void *data, size_t size, struct bt_media_path *out)
{
	const unsigned char *p = data;
	struct bt_media_path result = {0};
	size_t off = 0, len;
	unsigned nodes = 0, i, nonzero;
	int found = 0;

	if (p == NULL || out == NULL || size > BT_MEDIA_PATH_MAX)
		return (-1);
	while (off < size && ++nodes <= BT_MEDIA_NODES_MAX) {
		if (size - off < 4)
			return (-1);
		len = p[off + 2] | (size_t)p[off + 3] << 8;
		if (len < 4 || len > size - off)
			return (-1);
		if (p[off] == 0x7f) {
			if (p[off + 1] != 0xff || len != 4 ||
			    off + len != size || !found)
				return (-1);
			*out = result;
			return (0);
		}
		if (p[off] == 4) {
			if (p[off + 1] == 1) {
				if (found || off == 0 || len != 42 ||
				    p[off + 40] != 2 || p[off + 41] != 2)
					return (-1);
				result.parent = p;
				result.parent_len = off;
				result.partition = bt_media_u32(p + off + 4);
				result.start = bt_media_u64(p + off + 8);
				result.sectors = bt_media_u64(p + off + 16);
				memcpy(result.guid, p + off + 24, 16);
				nonzero = 0;
				for (i = 0; i < 16; i++)
					nonzero |= result.guid[i];
				if (!nonzero || result.partition == 0 ||
				    result.start == 0 || result.sectors == 0 ||
				    result.sectors > UINT64_MAX - result.start)
					return (-1);
				found = 1;
			} else if (p[off + 1] == 4) {
				/* Loaded-image paths may append UTF-16 filenames. */
				if (!found || len < 6 || len % 2 != 0 ||
				    p[off + len - 2] != 0 || p[off + len - 1] != 0)
					return (-1);
			} else
				return (-1);
		} else if (found)
			return (-1);
		off += len;
	}
	return (-1);
}

int
bt_media_sibling(const struct bt_media_path *boot,
    const struct bt_media_path *candidate)
{

	if (boot->parent_len == 0 || boot->parent_len != candidate->parent_len ||
	    memcmp(boot->parent, candidate->parent, boot->parent_len) != 0 ||
	    boot->partition == candidate->partition ||
	    memcmp(boot->guid, candidate->guid, sizeof(boot->guid)) == 0)
		return (0);
	return (boot->start + boot->sectors <= candidate->start ||
	    candidate->start + candidate->sectors <= boot->start);
}
