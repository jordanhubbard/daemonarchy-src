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

size_t
bt_normal_record(char *out, size_t cap, const struct bt_normal_identity *id,
    const char *partition, unsigned slot)
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
	n = snprintf(out, cap, "Daemonarchy normal boot trace v1\n"
	    "image_id=%s\narchive_sha256=%s\ntrace_partition=%s\n"
	    "session=%04u\nphase=loader-ready\nidentity=declared-archive\n"
	    "early_kernel_backend=not-enabled\n",
	    id->image_id, id->archive_sha256, partition, slot);
	if (n < 0 || (size_t)n >= cap)
		return (0);
	for (i = 0; i < (size_t)n; i++)
		sum = (sum ^ (unsigned char)out[i]) * 16777619U;
	added = snprintf(out + n, cap - n, "sum=%08x\n", sum);
	if (added < 0 || (size_t)added >= cap - n)
		return (0);
	return ((size_t)n + added);
}
