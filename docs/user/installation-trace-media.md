# Installation trace media binding

The normal installation-tracing candidate needs to identify its own writable
trace volume before capture can be enabled. `boottrace media` is a read-only
EFI discovery command for that boundary. It does not capture a boot, consume an
experiment queue, save NVRAM, run an experiment or start a listener. Automatic
loader/kernel/installer capture remains unfinished under the product's TRACE-001.
The historical `boottrace cycle` workflow retains its existing semantics.

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

The [native build and exact EFI VM checkpoint](../../verification/trace-media-20261109.json)
records all six discovery outcomes separately from portable parser checks. This candidate does not yet change normal startup to capture
traces. Kernel/init handoff, durable per-boot records, image binding, writable
raw-map validation, failure recovery, amd64 and physical acceptance remain open.
