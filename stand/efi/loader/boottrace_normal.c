/* SPDX-License-Identifier: BSD-2-Clause */
/* Normal boot metadata and allocation policy; no experiment state is used. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "boottrace_normal.h"

static int
normal_hex(const char *p, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (!((p[i] >= '0' && p[i] <= '9') ||
		    (p[i] >= 'a' && p[i] <= 'f')))
			return (0);
	return (1);
}

static int
normal_uuid(const char *p)
{
	size_t i;

	for (i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (p[i] != '-')
				return (0);
		} else if (!normal_hex(p + i, 1))
			return (0);
	}
	return (1);
}

/* A missing, failed or trivially stuck RNG is not a fresh boot identity. */
int
bt_normal_nonce(char *out, size_t cap, const uint8_t *bytes, size_t size)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	if (out == NULL || cap < BT_NORMAL_NONCE_SIZE || bytes == NULL ||
	    size != BT_NORMAL_NONCE_BYTES)
		return (-1);
	for (i = 1; i < size; i++)
		if (bytes[i] != bytes[0])
			break;
	if (i == size)
		return (-1);
	for (i = 0; i < size; i++) {
		out[2 * i] = hex[bytes[i] >> 4];
		out[2 * i + 1] = hex[bytes[i] & 15];
	}
	out[2 * size] = '\0';
	return (0);
}

static int
normal_nonce_valid(const char *nonce)
{
	size_t i, length;

	if (nonce == NULL)
		return (0);
	for (length = 0; length < BT_NORMAL_NONCE_SIZE && nonce[length] != '\0';
	    length++)
		;
	if (length == sizeof(BT_NORMAL_NONCE_UNAVAILABLE) - 1 &&
	    memcmp(nonce, BT_NORMAL_NONCE_UNAVAILABLE, length) == 0)
		return (1);
	if (length != BT_NORMAL_NONCE_SIZE - 1 || !normal_hex(nonce, length))
		return (0);
	for (i = 2; i < length; i++)
		if (nonce[i] != nonce[i % 2])
			return (1);
	return (0);
}

int
bt_normal_identity(const char *data, size_t size,
    struct bt_normal_identity *out)
{
	static const char head[] = BT_NORMAL_ID_TAG "image_id=";
	static const char middle[] = "\narchive_sha256=";
	static const char tail[] = "\nmode=observe-only\n";
	size_t off = sizeof(head) - 1;
	struct bt_normal_identity result;

	if (data == NULL || out == NULL || size != sizeof(head) - 1 + 36 +
	    sizeof(middle) - 1 + 64 + sizeof(tail) - 1 ||
	    size > BT_NORMAL_ID_MAX || memcmp(data, head, off) != 0 ||
	    !normal_uuid(data + off))
		return (-1);
	memcpy(result.image_id, data + off, 36);
	result.image_id[36] = '\0';
	off += 36;
	if (memcmp(data + off, middle, sizeof(middle) - 1) != 0)
		return (-1);
	off += sizeof(middle) - 1;
	if (!normal_hex(data + off, 64))
		return (-1);
	memcpy(result.archive_sha256, data + off, 64);
	result.archive_sha256[64] = '\0';
	off += 64;
	if (memcmp(data + off, tail, sizeof(tail) - 1) != 0)
		return (-1);
	*out = result;
	return (0);
}

int
bt_normal_empty(const char *data, size_t size)
{
	static const char marker[] = BT_NORMAL_EMPTY;
	size_t i;

	if (data == NULL || size != BT_NORMAL_SIZE ||
	    memcmp(data, marker, sizeof(marker) - 1) != 0)
		return (0);
	for (i = sizeof(marker) - 1; i < size; i++)
		if (data[i] != '\n')
			return (0);
	return (1);
}

static size_t
normal_record(char *out, size_t cap, const struct bt_normal_identity *id,
    const char *partition, unsigned slot, const char *tag, const char *phase,
    const char *nonce)
{
	uint32_t sum = 2166136261U;
	size_t i;
	int n, added;

	if (out == NULL || id == NULL || partition == NULL ||
	    id->image_id[36] != '\0' || id->archive_sha256[64] != '\0' ||
	    strlen(partition) != 36 || !normal_uuid(partition) ||
	    !normal_uuid(id->image_id) || !normal_hex(id->archive_sha256, 64) ||
	    slot == 0 || slot > BT_NORMAL_SLOTS)
		return (0);
	n = snprintf(out, cap, "%s"
	    "image_id=%s\narchive_sha256=%s\ntrace_partition=%s\n"
	    "session=%04u\nphase=%s\nidentity=declared-archive\n"
	    "early_kernel_backend=not-enabled\n",
	    tag, id->image_id, id->archive_sha256, partition, slot, phase);
	if (n < 0 || (size_t)n >= cap)
		return (0);
	if (nonce != NULL) {
		added = snprintf(out + n, cap - n, "boot_nonce=%s\nboot_identity=%s\n",
		    nonce, strcmp(nonce, BT_NORMAL_NONCE_UNAVAILABLE) == 0 ?
		    "unavailable" : "firmware-rng");
		if (added < 0 || (size_t)added >= cap - n)
			return (0);
		n += added;
	}
	for (i = 0; i < (size_t)n; i++)
		sum = (sum ^ (unsigned char)out[i]) * 16777619U;
	added = snprintf(out + n, cap - n, "sum=%08x\n", sum);
	if (added < 0 || (size_t)added >= cap - n)
		return (0);
	return ((size_t)n + added);
}


/* Retain the legacy encoder for historical records and existing callers. */
size_t
bt_normal_record(char *out, size_t cap, const struct bt_normal_identity *id,
    const char *partition, unsigned slot)
{
	return (normal_record(out, cap, id, partition, slot,
	    "Daemonarchy normal boot trace v1\n", "loader-ready", NULL));
}

static size_t
normal_stage_record(char *out, size_t cap,
    const struct bt_normal_identity *id, const char *partition, unsigned slot,
    enum bt_normal_stage stage, const char *nonce, unsigned version)
{
	const char *phase;

	switch (stage) {
	case BT_NORMAL_STAGE_READY:
		phase = "loader-ready";
		break;
	case BT_NORMAL_STAGE_PREEXIT:
		phase = "pre-exit";
		break;
	default:
		return (0);
	}
	if (version < 2 || version > 4 ||
	    (version == 2 ? nonce != NULL : !normal_nonce_valid(nonce)))
		return (0);
	if (cap > BT_NORMAL_STAGE_SIZE)
		cap = BT_NORMAL_STAGE_SIZE;
	return (normal_record(out, cap, id, partition, slot,
	    version == 2 ? "Daemonarchy normal boot trace v2\n" :
	    version == 3 ? "Daemonarchy normal boot trace v3\n" :
	    "Daemonarchy normal boot trace v4\n", phase, nonce));
}

size_t
bt_normal_stage_record(char *out, size_t cap,
    const struct bt_normal_identity *id, const char *partition, unsigned slot,
    enum bt_normal_stage stage)
{
	return (normal_stage_record(out, cap, id, partition, slot, stage, NULL, 2));
}

size_t
bt_normal_boot_record(char *out, size_t cap,
    const struct bt_normal_identity *id, const char *partition, unsigned slot,
    enum bt_normal_stage stage, const char *nonce)
{
	if (nonce == NULL)
		return (0);
	return (normal_stage_record(out, cap, id, partition, slot, stage, nonce, 3));
}

/* v4 reserves the remaining two aligned banks for independently bound
 * userspace evidence. The legacy v1/v2/v3 entry points retain their formats. */
size_t
bt_normal_session_record(char *out, size_t cap,
    const struct bt_normal_identity *id, const char *partition, unsigned slot,
    enum bt_normal_stage stage, const char *nonce)
{
    return (normal_stage_record(out, cap, id, partition, slot, stage, nonce, 4));
}

static int
normal_padding(const char *data, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++)
		if (data[i] != '\n')
			return (0);
	return (1);
}

/* Compare complete canonical records, including padding. Corrupt/partial later
 * data is never an empty bank and must never be overwritten on a retry. */
static int
normal_preexit_state(const char *data, size_t size,
    const struct bt_normal_identity *id, const char *partition, unsigned slot,
    const char *nonce, unsigned version)
{
	char expected[1024];
	size_t n;

	if (data == NULL || size != BT_NORMAL_SIZE)
		return (-1);
	n = normal_stage_record(expected, sizeof(expected), id, partition,
	    slot, BT_NORMAL_STAGE_READY, nonce, version);
	if (n == 0 || memcmp(data, expected, n) != 0 ||
	    !normal_padding(data + n, BT_NORMAL_STAGE_SIZE - n) ||
	    !normal_padding(data + BT_NORMAL_PREEXIT_OFF + BT_NORMAL_STAGE_SIZE,
	    BT_NORMAL_SIZE - BT_NORMAL_PREEXIT_OFF - BT_NORMAL_STAGE_SIZE))
		return (-1);
	data += BT_NORMAL_PREEXIT_OFF;
	if (normal_padding(data, BT_NORMAL_STAGE_SIZE))
		return (0);
	n = normal_stage_record(expected, sizeof(expected), id, partition,
	    slot, BT_NORMAL_STAGE_PREEXIT, nonce, version);
	if (n == 0 || memcmp(data, expected, n) != 0 ||
	    !normal_padding(data + n, BT_NORMAL_STAGE_SIZE - n))
		return (-1);
	return (1);
}

int
bt_normal_preexit_state(const char *data, size_t size,
    const struct bt_normal_identity *id, const char *partition, unsigned slot)
{
	return (normal_preexit_state(data, size, id, partition, slot, NULL, 2));
}

int
bt_normal_boot_preexit_state(const char *data, size_t size,
    const struct bt_normal_identity *id, const char *partition, unsigned slot,
    const char *nonce)
{
	if (nonce == NULL)
		return (-1);
	return (normal_preexit_state(data, size, id, partition, slot, nonce, 3));
}


int
bt_normal_session_preexit_state(const char *data, size_t size,
    const struct bt_normal_identity *id, const char *partition, unsigned slot,
    const char *nonce)
{
    return (normal_preexit_state(data, size, id, partition, slot, nonce, 4));
}
