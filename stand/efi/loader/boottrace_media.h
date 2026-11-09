/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _BOOTTRACE_MEDIA_H_
#define _BOOTTRACE_MEDIA_H_

#define BT_MEDIA_PATH_MAX 4096
#define BT_MEDIA_NODES_MAX 64

struct bt_media_path {
	const unsigned char *parent;
	size_t parent_len;
	uint64_t start, sectors;
	uint32_t partition;
	unsigned char guid[16];
};

/* Only a complete, single-instance GPT hard-drive path is admitted. */
int bt_media_parse(const void *, size_t, struct bt_media_path *);
/* Distinct, nonoverlapping partitions on exactly the same physical path. */
int bt_media_sibling(const struct bt_media_path *, const struct bt_media_path *);

#endif
