# ACPI xHCI DMA reachability with full system RAM

The uncapped 8 GiB Pi4 simulator reaches xHCI command timeouts with the unchanged
pftf 1.53 firmware and the prior kernel. Its command ring, device-context array
and event ring were placed above 4 GiB. The programmed inbound PCIe BAR2 window
is 4 GiB, while the firmware's ACPI contract is narrower: SCB0._DMA declares
only the first 3 GiB. Limiting total system RAM had hidden this driver gap.

A disposable control with the old kernel, 8 GiB exposed, the firmware RAM cap
disabled and `hw.usb.xhci.dma32=1` passed the 19 USB acceptance checks. That
supports DMA reachability as the cause. A 32-bit quirk alone does not honor the
3 GiB ACPI aperture, so it is not the final repair.

## Resource contract and implementation

The running firmware namespace identifies `\_SB_.SCB0.XHC0` as HID PNP0D10,
UID0, no CID. Its parent is ACPI0004, UID0. These are generic identifiers; this
change does not guess a board from them. The firmware
[Xhci.asl at edk2-platforms 08566230](https://github.com/tianocore/edk2-platforms/blob/08566230c98729eb9760211e3111c7b4c391ccbf/Platform/RaspberryPi/AcpiTables/Xhci.asl)
and the actual runtime RPI4XHCI SSDT both declare the zero-based identity range
ending at `0xbfffffff`, length `0xc0000000`.

The runtime SSDT is 580 bytes, SHA256
`f95dbcedade83d2159c61ef5f20c1325b336c8c351fdc212c70a086b77dce676`.
The retained test fixture contains its exact 46-byte QWordMemory descriptor,
at offset204 after the _DMA name, together with that source identity. It
contains no host-specific capture paths.

The ACPI frontend walks namespace parents to the nearest _DMA and accepts one
fixed, zero-based, untranslated memory aperture with a consistent nonzero,
page-aligned length. This prevents bus-DMA from rounding an aperture endpoint
up beyond the firmware limit. It requires a corresponding _CRS. Empty,
translated, disjoint,
non-memory, non-fixed and malformed resource sets fail attachment explicitly.
The initial supported grammar is deliberately bounded. With no _DMA, the
inherited DMA tag remains selected. This follows the standard resource purpose
in [ACPI 6.6, device configuration](https://uefi.org/specs/ACPI/6.6/06_Device_Configuration.html#dma-direct-memory-access).

For an accepted aperture the frontend creates an owned bus-DMA parent tag.
xHCI uses it for its USB DMA-tag hierarchy, including controller rings,
contexts and transfer buffers. Bus-DMA can allocate or bounce below the actual
aperture limit while FreeBSD uses all available system RAM. No global RAM cap,
controller capability lie, firmware edit or simulator window widening is used.
The existing PCI and FDT frontends keep their inherited tag selection.

## Lifetime and failure handling

The frontend transfers its owned tag to the generic xHCI lifecycle. USB children
are detached, the owned-tag controller is halted, and its interrupt handler is
removed before mappings and parent tag are freed. The owned-tag frontend also
keeps an independent Newbus USB-child handle: real USB detach clears the bus's
bdev pointer while leaving that child and its parent-softc ivars alive. Cleanup
must successfully delete this saved child before releasing final resources;
deleting it unsuccessfully retains the handle and enters quarantine. Halt,
interrupt teardown or
resource-release failures preserve the remaining resources for a retry. A busy
child preserves the running controller and all its owners.

Newbus releases a driver's softc when attach returns an error. If cleanup of a
failed owned-tag attach cannot finish, the driver therefore keeps its Newbus
attachment, records the original failure, and prints `USB unavailable` with both
errors. This state retains owners for a later detach retry; it does not claim
usable USB. Partial cleanup failures after a previously successful attachment
also enter this state. Unattached USB children are disabled and later-pass,
driver-added, rescan and power callbacks cannot reattach or revive them. A normal
failure with completed cleanup still returns an error.
The no-override attach path retains its existing failure policy.

Core initialization flags distinguish successful USB DMA setup from command
lock initialization. Failed USB DMA allocation already unwinds itself. Early
failures and repeated cleanup consequently avoid freeing uninitialized or
already freed DMA objects and locks. The guards also apply to the existing PCI
and FDT consumers of xhci_init/xhci_uninit.

## Verification and remaining acceptance

```
python3 tools/daemonarchy/tests/xhci-acpi-dma.py --cc cc
```

The control compiles production ACPI parser, ancestor-walk and frontend bodies,
generic failure/detach bodies and xhci_uninit against bounded ACPICA, Newbus and
bus-DMA services. Input fields come from the exact captured descriptor. Cases
cover its 3 GiB limit, a higher ancestor, no _DMA, malformed/unsupported
resources, DMA-tag allocation failure, failures before and after core
initialization, child/halt/interrupt/resource-release failures, retained failed
attachment, busy parent-tag destruction after a previously successful attach,
partial-page
resource rejection, disabled child/later-pass/power guards, and retry without
double teardown. It models USB detach clearing bdev while the Newbus child and
its ivars remain; failed deletion preserves them and a later retry deletes the
child exactly once before DMA teardown.
These controls prove authored control flow, not the kernel bus-DMA allocator or
hardware behavior.

The [integrated native build](pi5-sd-providers.md#integrated-native-build-and-pi4-compatibility-controls)
compiles and links this implementation in a fresh FreeBSD ARM64 GENERIC kernel,
SHA256 `8ec36736678f4de5308164fe6dfe990119b6ff042bf6eec6887ddd3ee84c60c9`.
The unchanged 2 GiB UEFI/USB control passes all 19 checks. A fresh uncapped
8 GiB model boot passes all 20 checks, including durable guest-memory bounds,
with no `hw.physmem` or `hw.usb.xhci.dma32` override. Dmesg reports the actual
ACPI aperture ending at `0xbfffffff`; all seven probes, clean USB/UFS storage,
four active CPUs, ten durable files and synchronized reset pass. Original
image bytes remain unchanged. Guest `hw.physmem` is 8,419,028,992 bytes;
full-memory stress and physical hardware remain open.
No physical Pi was changed or tested.

A second fresh 20-check run adds only read-only QMP physical-memory reads; it
also passes without stopping the guest or changing guest memory/registers.
Seventy-six complete post-kernel-handoff samples show command ring
`0x315d5f00`, device-context array `0x395d7000`, event-ring segment table
`0x315d5000`, and event dequeue pointers through `0x315d5d20`, all below
`0xc0000000`. The controller is running (`USBCMD=0xd`, status zero), and
firmware BAR2 remains `0x11` with high word zero. This demonstrates actual
controller DMA reachability during the full-memory bootstrap; it is not a
sustained whole-RAM or peripheral stress qualification.
