# BCM2711 ACPI property mailbox candidate

The Pi 4 ACPI firmware exposes the VideoCore property mailbox as `BCM2849`
at `0xfe00b880`. The existing simplebus driver does not attach to this ACPI
device, so the presence of `/dev/vcio` alone does not establish working
firmware properties. This candidate adds an ARM64 ACPI frontend to the original
mailbox driver. It is a prerequisite for firmware services, rather than a
Wi-Fi or accelerated graphics driver.

The frontend accepts only the BCM2711 mailbox register window with sufficient
space for the write register. It selects the existing BCM2711 VideoCore address
mapping explicitly and refuses a conflicting cached SoC selection. A matching
HID alone does not select a DMA domain. Pi 5 and other resource windows remain
unsupported by this frontend.

Some ACPI firmware supplies no mailbox IRQ. In that case the driver uses bounded
polling; it also bounds the cold-boot receive loop and initial FIFO drain.
Locks are initialized before interrupt registration. The original FDT frontend
remains present.

Property requests pair the DMA read/write synchronization operations and return
transport errors. When an already-sent request times out or returns the wrong
buffer address, firmware may still own the allocation. The driver retains that
one allocation until reboot, marks the provider failed and refuses subsequent
requests. It does not free memory that an unresponsive firmware transaction
could still overwrite. An unsent request can release its allocation normally.

## Development controls

Run the source-function fixture with an explicit new output directory:

```sh
python3 tools/daemonarchy/tests/pi4-acpi-mailbox-controls.py \
  --sanitize --work-dir /absolute/new/mailbox-controls
```

The fixture compiles five verbatim production functions against userspace
service stubs. Sixteen cases cover resource and SoC refusal, DMA-address bounds,
provider absence, allocation failure, unsent/completed requests, timeout and
wrong-response buffer retention, bounded receive loops, and IRQ message delivery.
All sixteen pass with address and undefined-behavior sanitizers on native
FreeBSD ARM64 and the Linux controller. Replacing only the property function
with its original implementation makes three safety assertions fail in the
same native fixture. These are function-level controls, not hardware emulation.
Generated source, binaries, logs and failed preparation attempts remain retained.

The exact three driver files also pass native kernel compilation and ELF linkage
and a complete ARM64 GENERIC kernel build using the recorded F4 source selection.
All 115,681 original and candidate source entries pass before/after checks;
the original bootstrap and worker host remain unchanged. The resulting kernel
boots in generic ARM64 UEFI guests and runs the software display controls.
That platform has no BCM2711 mailbox and therefore supplies no positive firmware
transaction result. See the [scoped development record](../verification/pi4-acpi-mailbox-20261122.json)
for source and product identities.

## Outstanding acceptance

Physical BCM2711 attachment, a read-only firmware-revision request, timeout
recovery on real hardware, and the original FDT/ARM32 paths remain unqualified.
The physical Pi 4 still runs its previous kernel and desktop. Firmware power,
GPIO, MMCCAM/SDIO enumeration, Wi-Fi association/traffic/recovery and VC4/V3D
acceleration require further work. The native builds and fixtures do not admit
the source/toolchain closure, earn a canonical lifecycle receipt, or complete
1.6 release acceptance.
