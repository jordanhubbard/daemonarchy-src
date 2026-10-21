# Wired UEFI loader console

- **Status:** active
- **Owning queue item:** [SRC-DEBUG-001](../roadmap/active-work.md); source issue #6
- **Acceptance:** pending actual FreeBSD EFI binary, UEFI VM and physical tests

The target is FreeBSD. An opt-in console must retain the local console and feed
authenticated input into the ordinary loader interpreter. `show`, `set` and
`boot` therefore keep their loader semantics. A missing NIC or peer must not
prevent local boot. Firmware networking ends at ExitBootServices; this console
does not establish early-kernel network debugging.

## Packet contract

The initial packet core is implemented in `stand/efi/loader/netconsole_proto.*`.
It is not yet connected to a console or network driver.

All multibyte fields use network byte order. Packets contain a 32-byte header,
at most 512 payload bytes and a trailing 32-byte HMAC-SHA256 tag. The header is
`LDNC` (4 bytes), version 1 (1), type (1), payload length (2), sequence (8) and
session identifier (16). The tag authenticates the entire header and payload
with a 32-byte key. Unknown types/versions, truncated or trailing bytes, wrong
sessions, invalid lengths and invalid tags refuse before input is admitted.

HELLO has sequence zero and no payload. INPUT and OUTPUT have a nonzero sequence
and nonempty payload. ACK and FINISH have nonzero sequence and no payload.
Each direction owns its sequence space. An INPUT is eligible only when it is
the next sequence. An identical most-recent packet may be acknowledged again
without executing its bytes twice; gaps, older records, changed content at the
same sequence and sequence wrap refuse. The console commits the sequence only
after the entire input fits its bounded queue.

The console integration must supply a fresh per-activation session identifier,
explicit key provisioning/revocation, bounded output retention and retransmission,
and nonblocking polling. Pairing must prove liveness with an authenticated
response to a fresh controller challenge; a replayed advertisement alone cannot
identify the current boot. That pairing exchange remains integration work.
An authenticated packet is not an encrypted packet:
the transport is intended for an explicitly selected trusted wired debugging
network, and must document its confidentiality limitations. No secret key or raw
private console capture belongs in source control.

## Native protocol check

Compile on an allocated FreeBSD worker, with objects outside the checkout:

```sh
cc -std=c99 -Wall -Wextra -Werror -I "$SRC/sys" \
  -I "$SRC/stand/efi/loader" \
  "$SRC/stand/efi/loader/netconsole_proto.c" \
  "$SRC/tools/daemonarchy/tests/netconsole-protocol.c" \
  -lmd -o "$OBJ/netconsole-protocol-test"
"$OBJ/netconsole-protocol-test"
```

This checks an independent HMAC vector, every-byte mutation, exact bounds and
input ordering/replay refusal. It does not qualify NIC behavior, packet loss,
firmware polling, an EFI binary, keyboard interpretation or kernel handoff.
