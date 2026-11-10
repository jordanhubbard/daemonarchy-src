# Automatic installation-media loader checkpoint

Normal media with `daemonarchy.trace.enable="YES"` now runs `boottrace observe`
automatically after loader configuration is read, before displaying the menu.
It saves a bounded `loader-ready` checkpoint to its own writable DATRACE volume.
The new v2 implementation also attempts an independent pre-exit checkpoint
before memory-map acquisition, after renewing the owned-medium admission.
It takes precedence over conflicting historical experiment/autocycle flags;
capture failure reports a status and lets normal startup continue. It does not
consume QUEUE, rewrite STATE, save NVRAM, alter tunables, start listeners or reboot.
The historical explicitly selected `boottrace cycle` workflow remains available.

This is the first stage of TRACE-001, not complete boot or installation tracing.
The record explicitly says `early_kernel_backend=not-enabled`. It does not
measure the selected kernel/modules, establish successful ExitBootServices,
retain kernel console/panic data, or capture init/installer progress yet. A successful checkpoint cannot establish
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
policy and FAT readers have 28 and 19 sanitizer controls respectively:

```sh
sh tools/daemonarchy/tests/run-boottrace-normal.sh "$PWD" /private/new-test-objects
```

Four Lua dispatcher controls also run through `run-loader-debug-setup.py` and
native `flua`. Native ARM64 loader-only VM tests verify cold process restart and
a controlled warm reset after a flushed record. Synthetic partial data tests
retention, not actual interrupted writes or physical power loss. Final default
media, amd64 firmware/BIOS, kernel/init/installer and physical acceptance remain
open. The earlier full RAM-root image does not inherit this new loader's result.


## Independent pre-exit checkpoint banks

The v2 writer keeps the existing 32 files, each exactly 16 KiB, and the existing
256-entry map limit. Loader-ready occupies the first 4 KiB bank; pre-exit uses
the next 4 KiB, leaving the remaining 8 KiB reserved. A later write must preserve
the complete earlier bank and reserved bytes. Legacy v1 records remain readable
and occupied legacy files are not reused.

The pre-exit hook runs before the first GetMemoryMap call, outside its retry
loop. It does no stand-filesystem open after device cleanup. It uses the identity
retained by a verified loader-ready capture, then renews the physical boot-medium
binding, declared archive identity, raw map, clean FAT metadata and all normal
file extents. A changed partition, read-only slot, invalid earlier bank or partial
later bank refuses a write. An identical completed later record is idempotent.
Only a derived 4 KiB subextent may change; whole-disk flush/readback and a fresh
read of the complete slot must pass before reporting success.

A pre-exit record means that the callback was reached. It does not prove that
firmware exit or kernel entry succeeded. Diagnostics and environment allocation
finish before acquiring the memory-map key; no capture, output or allocation is
added between the final map and ExitBootServices, including the retry path.
The volatile pre-exit status is loader-side information and is not claimed as a
post-copy kernel environment handoff.

The product controller decodes v1 and v2. An intact v2 loader-ready bank remains
available if the later bank is corrupt, partial or foreign; extraction reports
the later checkpoint unreadable and keeps the exact complete slot bytes. Unknown
writes into reserved space are refused. This is checksummed corruption detection,
not authentication or physical power-loss qualification.

Portable policy and block-writer controls cover the new format, immutable parent
and reserved banks, differing identities, truncated/partial data, and 512/4096-byte
logical media with aligned and half-block-offset files. The original native
amd64 EFI bootinfo fixture now asserts the callback's position and one-time use
across memory-map/exit failures and retries. Native EFI build/boot, actual
interrupted writes, amd64/BIOS integration and full kernel/init/installer/physical
acceptance remain required; controller and fake-block controls do not supply them.

## Native pre-exit development checkpoint

The [artifact-bound ARM64 checkpoint](../../verification/normal-preexit-20261110.json)
binds the exact fe12c0bb source, native unprivileged stand build and 1,384,876-byte
EFI to a minimal kernel-control medium. An automatic boot and fresh-process
restart each save both banks and enter the CURRENT kernel; only 607 bytes in
the newly allocated slot change. A synthetic partial later bank is retained
while the next session records normally. Read-only media and a trace-less
read-only primary beside a writable foreign trace disk boot without records
or changes to either disk. There is no network device or keyboard input.

The first foreign-disk run used a writable trace-less hybrid primary and changed
81 bytes in its backup GPT sectors, while leaving the foreign trace disk intact.
That run is retained and does not pass whole-primary preservation; attribution
to firmware/GPT repair remains unresolved. The retry deliberately holds the
primary read-only and does not establish that stronger writable-primary claim.

These controls stop at a deliberately unavailable root prompt. They do not run
init, an installer or a desktop and do not qualify physical hardware. The kernel
comes from the earlier af55 world build; only stand is rebuilt from fe12c0bb.
Native amd64 callback assertions now run in CI before the EFI build, including
sanitizers; their actual result must be checked separately. All remaining
TRACE-001 stages, final-media and release gates stay open.

The complete-GPT follow-up now also boots a writable trace-less primary beside
the writable foreign trace disk, with zero changed bytes on either image.
Both GPT header checksums and the identical entry arrays are independently
validated before boot. The original primary lacked its backup GPT; that first
run remains retained separately. Exact a486e309 native amd64 CI passes all three
handoff fixture groups in both plain and sanitized modes and builds the EFI.
The preserved outputs and input manifest match that source. This closes the
callback-fixture gate only; amd64 firmware boot, BIOS and physical gates remain.


## Fresh boot correlation nonce

The v3 normal writer requests 32 bytes from the firmware RNG once per loader
instance before its first admitted capture attempt. Failed-write retries keep
the same cached value. A successful nonuniform result is encoded as a lowercase
64-character boot_nonce with boot_identity=firmware-rng. Both independent banks
use the same cached nonce. The pre-exit writer validates that nonce against the
complete earlier record; a different boot's valid checksum cannot bind the
later checkpoint. No RNG request is made in the memory-map/firmware-exit window.

Missing/failed RNG and trivially stuck output produce boot_nonce=unavailable
and boot_identity=unavailable. There is no clock, archive, slot or counter
fallback pretending to be independent entropy. Ordinary capture and boot still
continue. After a verified ready write, daemonarchy.trace.boot_nonce is exported
for later stages before the kernel environment is copied.

The nonce is public correlation metadata. It is not authentication, stable
hardware identity or independent qualification of firmware entropy quality.
Stable target descriptors, canonical RNG/SDK admission and backends without
usable RNG remain separate requirements. Legacy v1/v2 records remain readable
and occupied; no preceding slot is rewritten. The file/map geometry and
independent checkpoint banks remain unchanged.

Forty portable policy and nineteen FAT-admission controls cover legacy retention,
nonce formatting, missing/stuck entropy, truncation, mismatched later nonces and
explicit unavailable state. Native construction and clone/restart controls must
be bound to their actual artifacts before claiming runtime freshness.

The [native correlation checkpoint](../../verification/normal-boot-identity-20261110.json)
binds source 75299834 to new ARM64 and amd64 EFI binaries. On both architectures,
two fresh processes boot the exact same image/archive/partition/session and
record different nonces, with the same nonce in both banks. Only 809 bytes in
the expected slot change. ARM fresh-process restart retains its earlier record
and allocates a new slot with a new nonce. Without the RNG device, both targets
persist an explicit unavailable identity, change only 701 slot bytes and still
enter the kernel. These are boot-only controls held at an unavailable root.
Kernel/userspace handoff querying and stable hardware identity remain open.
