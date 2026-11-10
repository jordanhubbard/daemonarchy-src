/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _BOOTTRACE_NORMAL_H_
#define _BOOTTRACE_NORMAL_H_

#include <stddef.h>
#include <stdint.h>

#define BT_NORMAL_SLOTS 32
#define BT_NORMAL_SIZE 16384
#define BT_NORMAL_STAGE_SIZE 4096
#define BT_NORMAL_PREEXIT_OFF BT_NORMAL_STAGE_SIZE
#define BT_NORMAL_NONCE_BYTES 32
#define BT_NORMAL_NONCE_SIZE 65
#define BT_NORMAL_NONCE_UNAVAILABLE "unavailable"

/* Separate aligned regions preserve the earlier checkpoint and map bound. */
enum bt_normal_stage {
	BT_NORMAL_STAGE_READY = 1,
	BT_NORMAL_STAGE_PREEXIT = 2
};
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

size_t bt_normal_stage_record(char *, size_t, const struct bt_normal_identity *,
    const char *, unsigned, enum bt_normal_stage);
/* -1: invalid/partial, 0: empty later region, 1: identical completed stage. */
int bt_normal_preexit_state(const char *, size_t,
    const struct bt_normal_identity *, const char *, unsigned);
int bt_normal_nonce(char *, size_t, const uint8_t *, size_t);
size_t bt_normal_boot_record(char *, size_t, const struct bt_normal_identity *,
    const char *, unsigned, enum bt_normal_stage, const char *);
int bt_normal_boot_preexit_state(const char *, size_t,
    const struct bt_normal_identity *, const char *, unsigned, const char *);
void boottrace_normal_preexit(void);

#endif
