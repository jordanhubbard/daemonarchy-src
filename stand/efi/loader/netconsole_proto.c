/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

#include <sys/types.h>
#include <sys/endian.h>
#include <crypto/sha2/sha256.h>
#include <string.h>

#include "netconsole_proto.h"

/* The tag covers the complete versioned header, session and bounded payload. */
static void
ldnet_hmac(const uint8_t *key, const void *data, size_t length, uint8_t *tag)
{
	SHA256_CTX ctx;
	uint8_t pad[SHA256_BLOCK_LENGTH], inner[SHA256_DIGEST_LENGTH];
	size_t i;

	memset(pad, 0x36, sizeof(pad));
	for (i = 0; i < LDNET_KEY_SIZE; i++)
		pad[i] ^= key[i];
	SHA256_Init(&ctx);
	SHA256_Update(&ctx, pad, sizeof(pad));
	SHA256_Update(&ctx, data, length);
	SHA256_Final(inner, &ctx);
	memset(pad, 0x5c, sizeof(pad));
	for (i = 0; i < LDNET_KEY_SIZE; i++)
		pad[i] ^= key[i];
	SHA256_Init(&ctx);
	SHA256_Update(&ctx, pad, sizeof(pad));
	SHA256_Update(&ctx, inner, sizeof(inner));
	SHA256_Final(tag, &ctx);
	explicit_bzero(pad, sizeof(pad));
	explicit_bzero(inner, sizeof(inner));
	explicit_bzero(&ctx, sizeof(ctx));
}

static int
ldnet_equal(const uint8_t *a, const uint8_t *b, size_t length)
{
	uint8_t different = 0;
	size_t i;

	for (i = 0; i < length; i++)
		different |= a[i] ^ b[i];
	return (different == 0);
}

static int
ldnet_fields(uint8_t type, uint64_t sequence, size_t length)
{
	if (length > LDNET_PAYLOAD_SIZE)
		return (0);
	switch (type) {
	case LDNET_HELLO:
		return (sequence == 0 && length == 0);
	case LDNET_ACK:
		return (sequence != 0 && length == 0);
	case LDNET_INPUT:
	case LDNET_OUTPUT:
		return (sequence != 0 && length != 0);
	case LDNET_FINISH:
		return (sequence != 0 && length == 0);
	default:
		return (0);
	}
}

size_t
ldnet_encode(uint8_t *packet, size_t capacity, const uint8_t *key,
    const uint8_t *session, uint8_t type, uint64_t sequence,
    const void *payload, size_t length)
{
	size_t size;

	if (packet == NULL || key == NULL || session == NULL ||
	    (length != 0 && payload == NULL) ||
	    !ldnet_fields(type, sequence, length))
		return (0);
	size = LDNET_HEADER_SIZE + length + LDNET_TAG_SIZE;
	if (capacity < size)
		return (0);
	memcpy(packet, "LDNC", 4);
	packet[4] = 1;
	packet[5] = type;
	be16enc(packet + 6, length);
	be64enc(packet + 8, sequence);
	memcpy(packet + 16, session, LDNET_SESSION_SIZE);
	if (length != 0)
		memcpy(packet + LDNET_HEADER_SIZE, payload, length);
	ldnet_hmac(key, packet, LDNET_HEADER_SIZE + length,
	    packet + LDNET_HEADER_SIZE + length);
	return (size);
}

int
ldnet_decode(const void *data, size_t size, const uint8_t *key,
    const uint8_t *session, struct ldnet_message *message)
{
	const uint8_t *packet = data;
	uint8_t tag[LDNET_TAG_SIZE];
	size_t length;
	uint64_t sequence;

	if (packet == NULL || key == NULL || session == NULL || message == NULL ||
	    size < LDNET_HEADER_SIZE + LDNET_TAG_SIZE ||
	    size > LDNET_PACKET_SIZE || memcmp(packet, "LDNC", 4) != 0 ||
	    packet[4] != 1)
		return (0);
	length = be16dec(packet + 6);
	sequence = be64dec(packet + 8);
	if (size != LDNET_HEADER_SIZE + length + LDNET_TAG_SIZE ||
	    !ldnet_fields(packet[5], sequence, length) ||
	    !ldnet_equal(packet + 16, session, LDNET_SESSION_SIZE))
		return (0);
	ldnet_hmac(key, packet, LDNET_HEADER_SIZE + length, tag);
	if (!ldnet_equal(tag, packet + LDNET_HEADER_SIZE + length, sizeof(tag))) {
		explicit_bzero(tag, sizeof(tag));
		return (0);
	}
	explicit_bzero(tag, sizeof(tag));
	message->type = packet[5];
	message->sequence = sequence;
	message->length = length;
	message->payload = packet + LDNET_HEADER_SIZE;
	message->tag = packet + LDNET_HEADER_SIZE + length;
	return (1);
}

enum ldnet_order
ldnet_order(const struct ldnet_replay *state, const struct ldnet_message *message)
{
	if (message->type != LDNET_INPUT || message->sequence == 0)
		return (LDNET_REJECT);
	if (state->sequence != UINT64_MAX &&
	    message->sequence == state->sequence + 1)
		return (LDNET_NEXT);
	if (state->sequence != 0 && message->sequence == state->sequence &&
	    ldnet_equal(state->tag, message->tag, LDNET_TAG_SIZE))
		return (LDNET_DUPLICATE);
	return (LDNET_REJECT);
}

/* Commit only after the complete input fits the console's bounded queue. */
void
ldnet_commit(struct ldnet_replay *state, const struct ldnet_message *message)
{

	state->sequence = message->sequence;
	memcpy(state->tag, message->tag, sizeof(state->tag));
}
