/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _BOOTTRACE_NORMAL_H_
#define _BOOTTRACE_NORMAL_H_

#define BT_NORMAL_SLOTS 32
#define BT_NORMAL_SIZE 16384
#define BT_NORMAL_ID_MAX 256
#define BT_NORMAL_ID_TAG "Daemonarchy installation trace identity v1\n"
#define BT_NORMAL_EMPTY "(empty normal boot slot)\n"

struct bt_normal_identity {
	char image_id[37];
	char archive_sha256[65];
};

int bt_normal_identity(const char *, size_t, struct bt_normal_identity *);
int bt_normal_empty(const char *, size_t);
size_t bt_normal_record(char *, size_t, const struct bt_normal_identity *,
    const char *, unsigned);

#endif
