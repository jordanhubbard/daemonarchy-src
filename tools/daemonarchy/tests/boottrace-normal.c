/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boottrace_normal.h"

static const char identity[] = BT_NORMAL_ID_TAG
    "image_id=01234567-89ab-cdef-0123-456789abcdef\n"
    "archive_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
    "mode=observe-only\n";

int
main(int argc, char **argv)
{
	FILE *fixture;
	struct bt_normal_identity id;
	char changed[sizeof(identity) + 2], record[1024], slot[BT_NORMAL_SIZE];
	uint32_t sum;
	uint8_t entropy[BT_NORMAL_NONCE_BYTES];
	char nonce[BT_NORMAL_NONCE_SIZE], other_nonce[BT_NORMAL_NONCE_SIZE];
	const char *end;
	size_t i, n;
	unsigned checks = 0;

	assert(argc == 1 || argc == 2);

	assert(bt_normal_identity(identity, sizeof(identity) - 1, &id) == 0);
	assert(strcmp(id.image_id, "01234567-89ab-cdef-0123-456789abcdef") == 0);
	checks++;
	for (i = 0; i < sizeof(identity) - 1; i++)
		assert(bt_normal_identity(identity, i, &id) != 0);
	checks++;
	assert(bt_normal_identity(NULL, sizeof(identity) - 1, &id) != 0);
	assert(bt_normal_identity(identity, sizeof(identity) - 1, NULL) != 0);
	checks++;
	memcpy(changed, identity, sizeof(identity));
	changed[sizeof(BT_NORMAL_ID_TAG) + 8] = 'X';
	assert(bt_normal_identity(changed, sizeof(identity) - 1, &id) != 0);
	checks++;
	memcpy(changed, identity, sizeof(identity));
	*strstr(changed, "abcdef\narchive") = 'A';
	assert(bt_normal_identity(changed, sizeof(identity) - 1, &id) != 0);
	checks++;
	memcpy(changed, identity, sizeof(identity));
	*strstr(changed, "archive_sha256=") = 'A';
	assert(bt_normal_identity(changed, sizeof(identity) - 1, &id) != 0);
	checks++;
	memcpy(changed, identity, sizeof(identity));
	strstr(changed, "archive_sha256=")[15] = 'g';
	assert(bt_normal_identity(changed, sizeof(identity) - 1, &id) != 0);
	checks++;
	memcpy(changed, identity, sizeof(identity));
	*strstr(changed, "observe-only") = 'x';
	assert(bt_normal_identity(changed, sizeof(identity) - 1, &id) != 0);
	checks++;
	assert(bt_normal_identity(identity, sizeof(identity), &id) != 0);
	checks++;
	memset(slot, '\n', sizeof(slot));
	memcpy(slot, BT_NORMAL_EMPTY, sizeof(BT_NORMAL_EMPTY) - 1);
	assert(bt_normal_empty(slot, sizeof(slot)));
	checks++;
	assert(!bt_normal_empty(slot, sizeof(slot) - 1));
	assert(!bt_normal_empty(NULL, sizeof(slot)));
	checks++;
	slot[sizeof(slot) - 1] = '\0';
	assert(!bt_normal_empty(slot, sizeof(slot)));
	checks++;
	slot[sizeof(slot) - 1] = '\n'; slot[1] = 'x';
	assert(!bt_normal_empty(slot, sizeof(slot)));
	checks++;
	assert(bt_normal_identity(identity, sizeof(identity) - 1, &id) == 0);
	n = bt_normal_record(record, sizeof(record), &id, id.image_id, 1);
	assert(n > 0 && n < sizeof(record));
	end = strstr(record, "sum="); assert(end != NULL);
	sum = 2166136261U;
	for (i = 0; record + i < end; i++) sum = (sum ^ (unsigned char)record[i]) * 16777619U;
	assert(strtoul(end + 4, NULL, 16) == sum);
	assert(strstr(record, "phase=loader-ready\n") != NULL);
	assert(!bt_normal_empty(record, n));
	checks++;
	assert(bt_normal_record(record, sizeof(record), &id, id.image_id, 0) == 0);
	assert(bt_normal_record(record, sizeof(record), &id, id.image_id, 33) == 0);
	checks++;
	assert(bt_normal_record(record, sizeof(record), &id, "invalid", 1) == 0);
	checks++;
	memset(record, 'Q', sizeof(record));
	assert(bt_normal_record(record, 1, &id, id.image_id, 1) == 0);
	assert(record[1] == 'Q');
	checks++;

	/* The bounded v2 layout retains legacy decoding and a separate bank. */
	memset(slot, '\n', sizeof(slot));
	n = bt_normal_stage_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY);
	assert(n > 0 && n < 512);
	assert(strstr(record, "normal boot trace v2\n") != NULL);
	memcpy(slot, record, n);
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == 0);
	checks++;
	for (i = 0; i < sizeof(slot); i++)
		assert(bt_normal_preexit_state(slot, i, &id, id.image_id, 1) == -1);
	assert(bt_normal_preexit_state(NULL, sizeof(slot), &id, id.image_id, 1) == -1);
	checks++;
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 2) == -1);
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id,
	    "fedcba98-7654-3210-fedc-ba9876543210", 1) == -1);
	checks++;
	slot[0] ^= 1;
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	slot[0] ^= 1;
	slot[BT_NORMAL_STAGE_SIZE - 1] = 'x';
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	slot[BT_NORMAL_STAGE_SIZE - 1] = '\n';
	checks++;
	slot[BT_NORMAL_PREEXIT_OFF] = 'p';
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	slot[BT_NORMAL_PREEXIT_OFF] = '\n';
	slot[sizeof(slot) - 1] = 'x';
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	slot[sizeof(slot) - 1] = '\n';
	checks++;
	n = bt_normal_stage_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_PREEXIT);
	assert(n > 0 && n < 512);
	assert(strstr(record, "phase=pre-exit\n") != NULL);
	memcpy(slot + BT_NORMAL_PREEXIT_OFF, record, n);
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == 1);
	checks++;
	slot[BT_NORMAL_PREEXIT_OFF + n - 2] ^= 1;
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	slot[BT_NORMAL_PREEXIT_OFF + n - 2] ^= 1;
	slot[BT_NORMAL_PREEXIT_OFF + BT_NORMAL_STAGE_SIZE - 1] = 'x';
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	slot[BT_NORMAL_PREEXIT_OFF + BT_NORMAL_STAGE_SIZE - 1] = '\n';
	checks++;
	/* A complete but differently bound later record is not resumable. */
	n = bt_normal_stage_record(record, sizeof(record), &id, id.image_id,
	    2, BT_NORMAL_STAGE_PREEXIT);
	memset(slot + BT_NORMAL_PREEXIT_OFF, '\n', BT_NORMAL_STAGE_SIZE);
	memcpy(slot + BT_NORMAL_PREEXIT_OFF, record, n);
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	checks++;
	assert(bt_normal_stage_record(record, sizeof(record), &id, id.image_id,
	    1, (enum bt_normal_stage)0) == 0);
	assert(bt_normal_stage_record(record, sizeof(record), &id, id.image_id,
	    1, (enum bt_normal_stage)99) == 0);
	assert(bt_normal_stage_record(record, 1, &id, id.image_id,
	    1, BT_NORMAL_STAGE_PREEXIT) == 0);
	checks++;
	memset(slot, '\n', sizeof(slot));
	n = bt_normal_record(record, sizeof(record), &id, id.image_id, 1);
	memcpy(slot, record, n);
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	checks++;

	/* v3 correlates stages using independent boot entropy, never image/slot. */
	for (i = 0; i < sizeof(entropy); i++)
		entropy[i] = (uint8_t)i;
	assert(bt_normal_nonce(nonce, sizeof(nonce), entropy, sizeof(entropy)) == 0);
	assert(strcmp(nonce, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") == 0);
	checks++;
	memset(other_nonce, 'Q', sizeof(other_nonce));
	assert(bt_normal_nonce(other_nonce, 64, entropy, sizeof(entropy)) == -1);
	assert(other_nonce[0] == 'Q' && other_nonce[64] == 'Q');
	assert(bt_normal_nonce(NULL, sizeof(nonce), entropy, sizeof(entropy)) == -1);
	assert(bt_normal_nonce(other_nonce, sizeof(other_nonce), NULL, sizeof(entropy)) == -1);
	assert(bt_normal_nonce(other_nonce, sizeof(other_nonce), entropy, sizeof(entropy) - 1) == -1);
	checks++;
	for (i = 0; i < 256; i++) {
		memset(entropy, (int)i, sizeof(entropy));
		assert(bt_normal_nonce(other_nonce, sizeof(other_nonce), entropy, sizeof(entropy)) == -1);
	}
	checks++;
	memset(slot, '\n', sizeof(slot));
	n = bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, nonce);
	assert(n > 0 && n < 1024);
	assert(strstr(record, "normal boot trace v3\n") != NULL);
	assert(strstr(record, "boot_identity=firmware-rng\n") != NULL);
	memcpy(slot, record, n);
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == 0);
	checks++;
	for (i = 0; i < sizeof(entropy); i++)
		entropy[i] = (uint8_t)(255 - i);
	assert(bt_normal_nonce(other_nonce, sizeof(other_nonce), entropy, sizeof(entropy)) == 0);
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, other_nonce) == -1);
	assert(bt_normal_preexit_state(slot, sizeof(slot), &id, id.image_id, 1) == -1);
	checks++;
	for (i = 0; i < sizeof(slot); i++)
		assert(bt_normal_boot_preexit_state(slot, i, &id, id.image_id, 1, nonce) == -1);
	assert(bt_normal_boot_preexit_state(NULL, sizeof(slot), &id, id.image_id, 1, nonce) == -1);
	checks++;
	slot[BT_NORMAL_STAGE_SIZE - 1] = 'x';
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == -1);
	slot[BT_NORMAL_STAGE_SIZE - 1] = '\n';
	slot[sizeof(slot) - 1] = 'x';
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == -1);
	slot[sizeof(slot) - 1] = '\n';
	checks++;
	n = bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_PREEXIT, nonce);
	memcpy(slot + BT_NORMAL_PREEXIT_OFF, record, n);
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == 1);
	checks++;
	n = bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_PREEXIT, other_nonce);
	memset(slot + BT_NORMAL_PREEXIT_OFF, '\n', BT_NORMAL_STAGE_SIZE);
	memcpy(slot + BT_NORMAL_PREEXIT_OFF, record, n);
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == -1);
	checks++;
	memset(slot, '\n', sizeof(slot));
	n = bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, BT_NORMAL_NONCE_UNAVAILABLE);
	assert(n > 0 && strstr(record, "boot_identity=unavailable\n") != NULL);
	memcpy(slot, record, n);
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, BT_NORMAL_NONCE_UNAVAILABLE) == 0);
	n = bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_PREEXIT, BT_NORMAL_NONCE_UNAVAILABLE);
	memcpy(slot + BT_NORMAL_PREEXIT_OFF, record, n);
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, BT_NORMAL_NONCE_UNAVAILABLE) == 1);
	checks++;
	assert(bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, NULL) == 0);
	assert(bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, "") == 0);
	memset(other_nonce, '0', 64); other_nonce[64] = '\0';
	assert(bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, other_nonce) == 0);
	memset(other_nonce, 'f', sizeof(other_nonce));
	assert(bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, other_nonce) == 0);
	checks++;
	assert(bt_normal_boot_record(record, 1, &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, nonce) == 0);
	assert(bt_normal_boot_record(record, sizeof(record), &id, id.image_id,
	    1, (enum bt_normal_stage)0, nonce) == 0);
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, NULL) == -1);
	checks++;
	/* The live producer uses v4 without changing legacy caller formats. */
	memset(slot, '\n', sizeof(slot));
	n = bt_normal_session_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, nonce);
	assert(n > 0 && strstr(record, "normal boot trace v4\n") != NULL);
	memcpy(slot, record, n);
	assert(bt_normal_session_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == 0);
	assert(bt_normal_boot_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == -1);
	checks++;
	n = bt_normal_session_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_PREEXIT, nonce);
	assert(n > 0 && strstr(record, "normal boot trace v4\n") != NULL);
	memcpy(slot + BT_NORMAL_PREEXIT_OFF, record, n);
	assert(bt_normal_session_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == 1);
	checks++;
	if (argc == 2) {
		fixture = fopen(argv[1], "wbx");
		assert(fixture != NULL);
		assert(fwrite(slot, 1, sizeof(slot), fixture) == sizeof(slot));
		assert(fclose(fixture) == 0);
	}

	slot[BT_NORMAL_PREEXIT_OFF] ^= 1;
	assert(bt_normal_session_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == -1);
	slot[BT_NORMAL_PREEXIT_OFF] ^= 1;
	slot[2 * BT_NORMAL_STAGE_SIZE] = 'x';
	assert(bt_normal_session_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, nonce) == -1);
	checks++;
	assert(bt_normal_session_record(record, sizeof(record), &id, id.image_id,
	    1, BT_NORMAL_STAGE_READY, NULL) == 0);
	assert(bt_normal_session_preexit_state(slot, sizeof(slot), &id, id.image_id, 1, NULL) == -1);
	checks++;
	id.image_id[0] = '\n';
	assert(bt_normal_record(record, sizeof(record), &id, "01234567-89ab-cdef-0123-456789abcdef", 1) == 0);
	checks++;
	printf("normal boot trace policy: %u controls passed\n", checks);
	return (0);
}
