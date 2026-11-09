# Automatic installation-media loader checkpoint

Normal media with `daemonarchy.trace.enable="YES"` now runs `boottrace observe`
automatically after loader configuration is read, before displaying the menu.
It saves a bounded `loader-ready` checkpoint to its own writable DATRACE volume.
It takes precedence over conflicting historical experiment/autocycle flags;
capture failure reports a status and lets normal startup continue. It does not
consume QUEUE, rewrite STATE, save NVRAM, alter tunables, start listeners or reboot.
The historical explicitly selected `boottrace cycle` workflow remains available.

This is the first stage of TRACE-001, not complete boot or installation tracing.
The record explicitly says `early_kernel_backend=not-enabled`. It does not
capture selected kernel/modules, pre-ExitBootServices outcome, kernel console,
panic, init or installer progress yet. A successful checkpoint cannot establish
that any of those later stages ran.

`boottrace media` remains a separate read-only discovery command. Its selector
also supplies the owned-medium binding used by `boottrace observe`.

The command takes the loaded EFI image's device handle as its boot identity.
It admits complete single-instance GPT hard-drive device paths, matches the
physical path before the partition node, and requires a distinct nonoverlapping
partition with a nonzero GUID and the exact `DATRACE.TAG` bytes. Another USB
disk's matching label or tag cannot supply the binding. Two eligible partitions
on the boot disk are ambiguous. It enumerates existing filesystem handles;
it does not call ConnectController to probe additional hardware.

| `daemonarchy.trace.status` | Meaning |
| --- | --- |
| `ready` | One matching writable partition; `daemonarchy.trace.partition` holds its canonical GPT partition UUID. |
| `readonly` | The matching partition's firmware Block I/O provider is read-only; no writable identity is exported. |
| `notfound` | No matching tagged sibling partition is exposed. |
| `ambiguous` | More than one eligible tagged sibling is exposed. |
| `unbound` | The boot device cannot be bound to an admitted GPT hard-drive path, including the optical control. |
| `unavailable` | Firmware discovery or environment export could not complete. |

Every invocation first removes the previous exported partition/status, so a
failed renewal cannot retain a preceding writable identity. Consumers must
require `ready` and independently validate the expected image/profile identity,
raw map, filesystem and write/readback before persisting evidence. A firmware
partition UUID alone is not an image checksum, kernel-driver qualification or
permission to mount a globally matching label.

Run the production parser controls on Linux or FreeBSD with compiler sanitizers:

```sh
mkdir /private/new-test-objects
sh tools/daemonarchy/tests/run-boottrace-media.sh "$PWD" /private/new-test-objects
```

The object directory must exist outside source and the test output must be new.
Seventeen controls cover valid siblings, foreign disks, cloned GUIDs, overlapping
partitions, every truncated input length, invalid geometry/GPT types, absent
parents, malformed/end-instance nodes, filenames and byte/node processing budgets.
These budgets constrain parsing work, not target CPU or RAM discovery.

The [original discovery checkpoint](../../verification/trace-media-20261109.json)
records six outcomes for the earlier read-only candidate. The subsequent
[normal-startup checkpoint](../../verification/normal-boot-trace-20261109.json)
records the exact native ARM64 EFI, automatic writes, restart/reset recovery,
partial-slot retention and thirteen zero-write refusal controls separately.

## Write admission and retention

The product EFI builder creates matching `/boot/trace-id` and `INSTALL.ID`,
with a fresh image UUID, actual input-archive SHA-256 and `mode=observe-only`.
This is a declared archive identity, not an EFI measurement of the running
kernel or whole image. Cloned media can share these identifiers and GPT UUIDs;
the image/slot session is not a globally unique physical-target identity.

Normal capture validates that identity, the bounded raw map, an available
whole-disk write/readback backend and actual FAT32 metadata. Fresh sector reads
check clean mirrored FATs, bounded directory chains, exact filenames and
contiguous complete extents for STATE, INSTALL.ID and all 32 NORMAL slots.
Map offsets must agree with those actual file extents. Equal empty contents
are insufficient: a reproduced checksummed map that swapped two slots is now
refused. This is relevant-file admission, not a whole-filesystem checker or
proof against every possible alias in an unrelated file's corrupt chain.

Only an exactly initialized empty 16 KiB `NORMAL/NNNN.TXT` may be used. Existing
records, corrupt records and partial data are retained; the first empty slot
receives a checksummed record, flush and fresh whole-disk readback. Repeated
calls after success in the same loader instance do not allocate another slot.
After 32 occupied slots capture reports `full`, preserving all prior evidence.
There is no automatic rotation. Legacy experiment trace reset does not erase
NORMAL records. Retrieve them before deliberately rebuilding media.

`daemonarchy.trace.capture` reports `saved` on verified persistence, or
`saved-handoff-unavailable` if the record persisted but session export failed.
Refusals include `media-unavailable`, `identity-invalid`, `identity-mismatch`,
`state-invalid`, `map-invalid`, `backend-unavailable`, `filesystem-unverified`,
`slots-invalid`, `slot-read-failed`, `slot-readonly`, `full`, `metadata-invalid`
and `write-unverified`. These describe capture, separately from discovery's
`daemonarchy.trace.status`. A failed or unverified write is never success.

Read back with the product controller's `stick.py normal TARGET --out FRESH_DIR`.
It preserves exact slot bytes, including partial/corrupt records, and writes a
hash-bearing extraction manifest without changing the volume. The production
policy and FAT readers have 18 and 19 sanitizer controls respectively:

```sh
sh tools/daemonarchy/tests/run-boottrace-normal.sh "$PWD" /private/new-test-objects
```

Four Lua dispatcher controls also run through `run-loader-debug-setup.py` and
native `flua`. Native ARM64 loader-only VM tests verify cold process restart and
a controlled warm reset after a flushed record. Synthetic partial data tests
retention, not actual interrupted writes or physical power loss. Final default
media, amd64 firmware/BIOS, kernel/init/installer and physical acceptance remain
open. The earlier full RAM-root image does not inherit this new loader's result.
