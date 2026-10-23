/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */

#include <sys/types.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "netconsole_proto.h"

int
main(void)
{
	static const uint8_t expected[LDNET_TAG_SIZE] = {
		0x8e, 0x35, 0xe5, 0x0d, 0xde, 0x1c, 0x7f, 0xb6,
		0x10, 0x43, 0xc7, 0x18, 0x84, 0x22, 0x8c, 0x38,
		0xd4, 0x98, 0xfd, 0x3e, 0xbc, 0xe1, 0xc0, 0xe0,
		0xd0, 0x73, 0xde, 0x82, 0xeb, 0x8c, 0x02, 0x92
	};
	uint8_t key[LDNET_KEY_SIZE], session[LDNET_SESSION_SIZE];
	uint8_t packet[LDNET_PACKET_SIZE + 1], saved[LDNET_PACKET_SIZE];
	uint8_t other[LDNET_PACKET_SIZE], payload[LDNET_PAYLOAD_SIZE + 1];
	static const uint8_t derived[16] = {
		0x28, 0x26, 0x02, 0x78, 0x30, 0x66, 0xe7, 0xfc,
		0x32, 0xb4, 0x32, 0xde, 0xc7, 0x60, 0x9e, 0xc1
	};
	uint8_t challenge[16], fresh[16], next[16];
	struct ldnet_message message;
	struct ldnet_replay state = { 0 };
	size_t size, i, n;

	for (i = 0; i < sizeof(key); i++)
		key[i] = i;
	for (i = 0; i < sizeof(session); i++)
		session[i] = 0xa0 + i;
	size = ldnet_encode(packet, sizeof(packet), key, session, LDNET_INPUT,
	    1, "show\n", 5);
	assert(size == LDNET_HEADER_SIZE + 5 + LDNET_TAG_SIZE);
	assert(memcmp(packet + size - LDNET_TAG_SIZE, expected, sizeof(expected)) == 0);
	assert(ldnet_decode(packet, size, key, session, &message));
	assert(message.type == LDNET_INPUT && message.sequence == 1);
	assert(message.length == 5 && memcmp(message.payload, "show\n", 5) == 0);
	assert(ldnet_order(&state, &message) == LDNET_NEXT);
	ldnet_commit(&state, &message);
	assert(ldnet_order(&state, &message) == LDNET_DUPLICATE);
	memcpy(saved, packet, size);

	/* Every byte, including header, session and tag, is authenticated. */
	for (i = 0; i < size; i++) {
		packet[i] ^= 1;
		assert(!ldnet_decode(packet, size, key, session, &message));
		packet[i] ^= 1;
	}
	for (i = 0; i < size; i++)
		assert(!ldnet_decode(packet, i, key, session, &message));
	assert(!ldnet_decode(packet, size + 1, key, session, &message));
	key[0] ^= 1;
	assert(!ldnet_decode(packet, size, key, session, &message));
	key[0] ^= 1;
	session[0] ^= 1;
	assert(!ldnet_decode(packet, size, key, session, &message));
	session[0] ^= 1;
	assert(ldnet_encode(other, size - 1, key, session, LDNET_INPUT,
	    1, "show\n", 5) == 0);
	assert(ldnet_encode(other, sizeof(other), key, session, LDNET_INPUT,
	    0, "show\n", 5) == 0);
	assert(ldnet_encode(other, sizeof(other), key, session, LDNET_INPUT,
	    1, payload, sizeof(payload)) == 0);
	assert(ldnet_encode(other, sizeof(other), key, session, LDNET_ACK,
	    1, "x", 1) == 0);
	assert(ldnet_encode(other, sizeof(other), key, session, 255,
	    1, "x", 1) == 0);
	assert(ldnet_encode(other, sizeof(other), key, session, LDNET_HELLO,
	    0, NULL, 0) == LDNET_HEADER_SIZE + LDNET_TAG_SIZE);

	/* Valid authentication does not permit sequence substitution or replay. */
	n = ldnet_encode(other, sizeof(other), key, session, LDNET_INPUT,
	    1, "boot\n", 5);
	assert(ldnet_decode(other, n, key, session, &message));
	assert(ldnet_order(&state, &message) == LDNET_REJECT);
	n = ldnet_encode(other, sizeof(other), key, session, LDNET_INPUT,
	    3, "show\n", 5);
	assert(ldnet_decode(other, n, key, session, &message));
	assert(ldnet_order(&state, &message) == LDNET_REJECT);
	n = ldnet_encode(other, sizeof(other), key, session, LDNET_INPUT,
	    2, "show\n", 5);
	assert(ldnet_decode(other, n, key, session, &message));
	assert(ldnet_order(&state, &message) == LDNET_NEXT);
	ldnet_commit(&state, &message);
	assert(ldnet_decode(saved, size, key, session, &message));
	assert(ldnet_order(&state, &message) == LDNET_REJECT);
	state.sequence = UINT64_MAX;
	assert(ldnet_order(&state, &message) == LDNET_REJECT);
	for (i = 0; i < sizeof(challenge); i++) challenge[i] = i;
	assert(!ldnet_session(key, session, 0, challenge, fresh));
	assert(ldnet_session(key, session, 1, challenge, fresh));
	assert(memcmp(fresh, derived, sizeof(fresh)) == 0);
	assert(ldnet_session(key, session, 2, challenge, next));
	assert(memcmp(fresh, next, sizeof(fresh)) != 0);
	challenge[0] ^= 1;
	assert(ldnet_session(key, session, 1, challenge, next));
	assert(memcmp(fresh, next, sizeof(fresh)) != 0);
	assert(!ldnet_decode(saved, size, key, fresh, &message));
	/* Pairing schemas and direction-specific ACKs cannot be substituted. */
	n = ldnet_encode(other, sizeof(other), key, session, LDNET_CHALLENGE,
	    0, challenge, 16);
	assert(ldnet_decode(other, n, key, session, &message));
	assert(message.type == LDNET_CHALLENGE);
	assert(ldnet_order(&state, &message) == LDNET_REJECT);
	assert(ldnet_encode(other, sizeof(other), key, session, LDNET_CHALLENGE,
	    1, challenge, 16) == 0);
	assert(ldnet_encode(other, sizeof(other), key, session, LDNET_WELCOME,
	    0, challenge, 16) == 0);
	n = ldnet_encode(other, sizeof(other), key, session, LDNET_WELCOME,
	    0, key, 32);
	assert(ldnet_decode(other, n, key, session, &message));
	n = ldnet_encode(other, sizeof(other), key, session, LDNET_ACK, 1, NULL, 0);
	assert(ldnet_decode(other, n, key, session, &message));
	assert(message.type != LDNET_OUTPUT_ACK);
	other[5] = LDNET_OUTPUT_ACK;
	assert(!ldnet_decode(other, n, key, session, &message));
	n = ldnet_encode(other, sizeof(other), key, session, LDNET_OUTPUT_ACK,
	    1, NULL, 0);
	assert(ldnet_decode(other, n, key, session, &message));
	assert(message.type == LDNET_OUTPUT_ACK);
	n = ldnet_encode(other, sizeof(other), key, session,
	    LDNET_KERNEL_WELCOME, 0, key, 32);
	assert(ldnet_decode(other, n, key, session, &message));
	assert(message.type == LDNET_KERNEL_WELCOME);
	other[5] = LDNET_WELCOME;
	assert(!ldnet_decode(other, n, key, session, &message));
	assert(ldnet_encode(other, sizeof(other), key, session,
	    LDNET_KERNEL_WELCOME, 1, key, 32) == 0);
	assert(ldnet_encode(other, sizeof(other), key, session,
	    LDNET_KERNEL_WELCOME, 0, key, 16) == 0);
	puts("netconsole protocol: HMAC/session vectors, mutation, bounds, roles and replay checks passed");
	return (0);
}
