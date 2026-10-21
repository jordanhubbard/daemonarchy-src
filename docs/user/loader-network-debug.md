# Wired UEFI loader console

- **Status:** implemented; native build and initial UEFI VM checks pass
- **Owning queue item:** [SRC-DEBUG-001](../roadmap/active-work.md); source issue #6
- **Release acceptance:** pending physical firmware/NIC and remaining fault checks

Daemonarchy targets FreeBSD only. This opt-in UDP console uses a wired NIC
exposed by UEFI Simple Network Protocol (SNP). It retains the local console and
feeds complete authenticated command lines into the ordinary loader interpreter.
`show`, `set` and `boot` keep their normal semantics. An OS-supported USB or
Thunderbolt Ethernet adapter is not necessarily visible to firmware. BIOS boot,
Wi-Fi, arbitrary USB NIC drivers and early-kernel networking are not supplied by
this change. Use `netdebug devices` to check firmware interface availability.

## Provision a debugging medium

Build the actual FreeBSD loader using the native `stand` Makefiles with
`LOADER_NET_SUPPORT=yes`; install the matching Lua startup files too. Stage the
candidate on a separate recovery medium before replacing an installed loader.
For the native development build (with `SRC` and an external `OBJ` directory
selected explicitly):

```sh
MAKEOBJDIRPREFIX="$OBJ" make -C "$SRC/stand" -j2 NO_ROOT=yes LOADER_NET_SUPPORT=yes obj
MAKEOBJDIRPREFIX="$OBJ" make -C "$SRC/stand" -j2 NO_ROOT=yes LOADER_NET_SUPPORT=yes all
```

The controller is `tools/daemonarchy/loader-console.py` and uses Python's standard
library. Controller host identity is separate from FreeBSD target qualification.

Generate a new private key and securely copy the same file to `/boot/debug.key`
on the debugging medium:

```sh
python3 tools/daemonarchy/loader-console.py --make-key /private/path/debug.key
```

The key file contains 32 raw bytes or 64 hexadecimal digits, optionally followed
by a newline. The controller requires a private regular file, with no group or
other permissions. Protect the medium physically; FAT cannot enforce Unix file
permissions. Never commit keys or private captures. Authentication does not encrypt
commands or output: use a trusted, explicitly selected wired debugging network.

For example, configure `/boot/loader.conf` on that medium:

```conf
netdebug_enable="YES"
netdebug.interface="0"
netdebug.ip="192.0.2.10"
netdebug.peer="192.0.2.20"
netdebug.keyfile="/boot/debug.key"
```

Replace these example addresses with distinct static IPv4 addresses on the same
wired link. The peer is the controller. The debug path does not perform DHCP or
routing discovery. Successful startup waits at the ordinary loader prompt,
including when the peer is absent. If startup fails, the normal local boot path
continues. `netdebug_enable` is disabled by default.

At the local prompt, the equivalent manual commands are:

```text
netdebug devices
netdebug start 0 192.0.2.10 192.0.2.20 /boot/debug.key
netdebug status
netdebug stop
```

`stop` revokes the session and clears in-memory key material. The key is read
from the file, never supplied through command arguments or environment values.
Remove the profile/key file from the medium to disable future activations.

## Connect and retain output

```sh
python3 tools/daemonarchy/loader-console.py 192.0.2.10 \
  --key-file /private/path/debug.key \
  --output /private/path/new-console.bin \
  --events /private/path/new-events.jsonl
```

UDP port 6668 must be reachable. Capture paths must be new; the controller creates
them privately and refuses overwriting existing files. Output capture contains
loader output with remote input echo suppressed; event capture contains protocol
metadata, not command text or keys. Commands that explicitly print private files
or values naturally put that output in the capture. Lines are limited to 255
printable ASCII bytes (tabs allowed). This is a line console, not a raw terminal.

For automation, repeat `--command`, for example `--command 'set boot_verbose=YES'`
and `--command 'show boot_verbose'`. An input acknowledgement proves queue
admission only. The controller subsequently sends a unique `echo` marker to
observe return to the ordinary interpreter. A timeout does not imply a crash,
power cycle or reboot. Reconnect with the same key to create a new session;
`netdebug.boot_id` remains stable within that loader boot.

Use `show boot.netif.hwaddr` to retain the firmware NIC's MAC address. The kernel
receives that value and `netdebug.boot_id` in its environment: `kenv
boot.netif.hwaddr` and `kenv netdebug.boot_id` can relate the native interface and
kernel boot to the controller's retained session. Match the MAC to `ifconfig`;
loader unit names do not promise native interface names. This identifies the
interface and boot, but does not activate kernel networking or a debugger.

`boot` ends firmware networking. The controller records
`firmware_handoff_started` with `kernel_observed: false` and exits **3**; normal
command completion exits **0** and errors/timeouts exit **1**. Confirm the kernel
through a separate console or native SSH. This does not establish a continuous
loader-to-kernel debugger. Kernel debugnet/NetGDB and native NIC initialization
need separate work and qualification.

## Packet and I/O contract

`stand/efi/loader/netconsole_proto.*` implements a 32-byte header, at most 512
payload bytes and a 32-byte HMAC-SHA256 tag. Multibyte fields use network byte
order: `LDNC` (4), version 1 (1), type (1), payload length (2), sequence (8),
session identifier (16). The tag authenticates the complete header and payload.
Exact sizes, schema, session and tag are checked before input is admitted.

Types are HELLO (1, reserved), INPUT (2), OUTPUT (3), INPUT_ACK (4), FINISH (5),
CHALLENGE (6), WELCOME (7), OUTPUT_ACK (8). Challenge uses sequence zero, a zero
header session and a fresh 16-byte controller nonce. The authenticated welcome
uses sequence zero, returns that nonce plus the stable random boot identifier,
and binds a session derived from the key, boot identifier, monotonic activation
counter and challenge. Fresh pairing revokes previous input and cancels a partial
command line. Repeated current challenges resend the welcome without resetting
input ordering. Firmware RNG or supported x86 RDRAND is required; failure refuses
activation while retaining local boot.

INPUT/OUTPUT have nonzero sequences and nonempty payloads. Acknowledgements and
FINISH have nonzero sequences and no payload. Each direction owns its sequence
space and acknowledgement type. Only the next complete command can enter the
input queue; an identical most-recent retry is acknowledged without re-execution.
Gaps, older packets, changed content at the same sequence and wrap refuse.
Commands exceeding the interpreter's line limit also refuse. Output is retained
in a bounded 16 KiB queue, with one packet awaiting acknowledgement and timed
retransmission; `netdebug status` exposes dropped output bytes.

The EFI driver uses one private transmit buffer and one completion owner for
both debug and ordinary network traffic after attachment. Pending firmware/DMA
memory is retained on timeout. Debug receive polling queues other frames for
normal netboot reads. At capacity it drops new ordinary frames, counts those
drops and continues bounded debug polling; queued frames remain available to
netboot readers. `netdebug status` reports normal RX drops as well as lost output. The default unattached path
retains ordinary networking behavior. Cleanup drains output for a bounded time,
sends FINISH and shuts down networking before the final memory-map/handoff path.
The native FreeBSD ELF boot path refuses handoff if the selected NIC is still
active or its shutdown failed, even without a pending transmit. A local error
requires a reboot to recover; pending DMA memory stays reserved in the loader.
Use the normal medium/profile after reboot if the firmware repeats the failure.
Alternate chain/hypervisor handoff paths require separate qualification.
Neither packet loss nor a stalled peer can disable local keyboard recovery.

## Verification and limits

The [development record](../../verification/loader-network-development.json)
binds source hashes and actual native observations. The integrated amd64 loader
build passes on FreeBSD 16.0-CURRENT with warnings as errors. An OVMF/QEMU UEFI
VM exercises authenticated output, `show`/`set`, interface/status commands,
reconnection, invalid input, lost output acknowledgement and acknowledgement
reflection. Shared-NIC TFTP reads and kernel loading also pass while debug is active.
Remote `boot net0:/boot/kernel/kernel` reaches a separately observed FreeBSD
login and SSH. A 128-packet unrelated UDP burst exercises 112 normal RX drops
without stranding debug commands.
Missing-peer local keyboard boot and missing-NIC normal boot also reach a
FreeBSD login. The stock kernel's IPv6 WITNESS warning is retained separately.

Compile the packet checks on an allocated FreeBSD worker, outside the checkout:

```sh
cc -std=c99 -Wall -Wextra -Werror -I "$SRC/sys" \
  -I "$SRC/stand/efi/loader" \
  "$SRC/stand/efi/loader/netconsole_proto.c" \
  "$SRC/tools/daemonarchy/tests/netconsole-protocol.c" \
  -lmd -o "$OBJ/netconsole-protocol-test"
"$OBJ/netconsole-protocol-test"
```

`tools/daemonarchy/tests/loader-console-live.py` checks the real protocol against
an owned disposable UEFI loader. It changes `ldnc_count`; never run it against
production hardware. Native packet checks include independent HMAC and session
vectors, every-byte mutation, bounds, pairing schemas and replay refusal.

Physical firmware/NIC support, broader shared-NIC/NFS regressions, extended
pressure and actual firmware behavior under failure remain release work. The
native callback fixture below separately verifies scripted failure handling. A passing VM or aggregate build does not qualify those cases,
a source-built SDK, canonical per-unit packages or a release ISO.

## Native firmware-fault checks

The fixture in `tools/daemonarchy/tests/efinet-console.c` includes the actual
`stand/efi/libefi/efinet.c` implementation. Scripted SNP and boot-service callbacks
control transmit refusal, completion timing, receive errors and shutdown failure;
unexpected initialization paths abort. This is a native FreeBSD amd64 logic
check, complementary to UEFI VM and physical firmware tests.

Run it with a new, existing object directory outside source:

```sh
sh "$SRC/tools/daemonarchy/tests/run-efinet-console.sh" "$SRC" "$OBJ" sanitize
```

The script compiles with warnings as errors and address/undefined-behavior
sanitizers. It refuses other OS/architecture targets, source-local objects and
an occupied binary path. Omit `sanitize` for a plain native run. The compiler
is `/usr/bin/cc` unless an explicit `CC` executable is selected. Its bootstrap
source provenance remains separate from the diagnostic result.

Checks cover copied transmit ownership, pending-buffer retention after timeout
or completion error, late completion, explicit reactivation, synchronous shared
transmit, failed-shutdown quarantine and handoff refusal, ordered normal RX retention, debug RX
consumption, queue overflow, allocator/receive failures, bounded polling and the
unattached networking path. Null/zero/oversized firmware modes and frame-size
integer overflow refuse; the overflow check first reproduced a failure in the
preceding implementation and then passed after correction.

FreeBSD's sanitizer runtime does not support LeakSanitizer. The fixture instead
asserts balanced owned RX allocations, including allocation failure, queue drain
and shutdown cleanup. Address and undefined-behavior errors still halt the test.
Passing these callback tests does not certify arbitrary physical DMA/firmware
behavior or establish a release lifecycle receipt.
