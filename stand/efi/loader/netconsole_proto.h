/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026 Daemonarchy contributors. */
#ifndef _LOADER_NETCONSOLE_PROTO_H_
#define _LOADER_NETCONSOLE_PROTO_H_

#include <sys/types.h>
#include <stdint.h>

#define LDNET_KEY_SIZE 32
#define LDNET_SESSION_SIZE 16
#define LDNET_HEADER_SIZE 32
#define LDNET_TAG_SIZE 32
#define LDNET_PAYLOAD_SIZE 512
#define LDNET_PACKET_SIZE (LDNET_HEADER_SIZE + LDNET_PAYLOAD_SIZE + LDNET_TAG_SIZE)

enum ldnet_type {
	LDNET_HELLO = 1,
	LDNET_INPUT,
	LDNET_OUTPUT,
	LDNET_ACK,
	LDNET_FINISH,
	LDNET_CHALLENGE,
	LDNET_WELCOME,
	LDNET_OUTPUT_ACK
};

struct ldnet_message {
	uint8_t type;
	uint64_t sequence;
	size_t length;
	const uint8_t *payload;
	const uint8_t *tag;
};

struct ldnet_replay {
	uint64_t sequence;
	uint8_t tag[LDNET_TAG_SIZE];
};

enum ldnet_order { LDNET_REJECT, LDNET_NEXT, LDNET_DUPLICATE };

size_t ldnet_encode(uint8_t *, size_t, const uint8_t *, const uint8_t *,
    uint8_t, uint64_t, const void *, size_t);
int ldnet_decode(const void *, size_t, const uint8_t *, const uint8_t *,
    struct ldnet_message *);
enum ldnet_order ldnet_order(const struct ldnet_replay *,
    const struct ldnet_message *);
void ldnet_commit(struct ldnet_replay *, const struct ldnet_message *);
int ldnet_session(const uint8_t *, const uint8_t *, uint64_t,
    const uint8_t *, uint8_t *);

#endif
