# Bounded ACPI namespace discovery

The original opt-in EFI discovery capture retains CPU/topology tables but omits
the DSDT and SSDTs that describe peripheral resources and wiring. The Pi 4
SDIO/GPIO investigation needs those actual firmware bytes before assigning
board policy. The reference firmware's
[Arasan](https://github.com/tianocore/edk2-platforms/blob/4db2845d14425599238be86b8a73d288f7c0b4a6/Platform/RaspberryPi/AcpiTables/Sdhc.asl)
and [eMMC2](https://github.com/tianocore/edk2-platforms/blob/4db2845d14425599238be86b8a73d288f7c0b4a6/Platform/RaspberryPi/AcpiTables/Emmc.asl)
descriptions both declare fixed children. That flag alone does not distinguish
the radio route from the physical card socket. No card-presence override or
GPIO/power write accompanies this diagnostic change.

The new bounded namespace reader follows the DSDT pointer only in a validated
FADT. A nonzero 64-bit X_DSDT supersedes the legacy 32-bit field; an invalid
extended pointer is reported rather than silently falling back. Tables remain
limited to 16 KiB and the root to 256 entries. Repeated reads must retain the
same signature and length, and the complete table checksum must pass. The
existing EFI memory-map reader still refuses MMIO, holes and read-protected
memory. No AML is evaluated.

Up to sixteen SSDTs are captured in root-table order as `SS00` through `SS0F`.
`SSOV` explicitly reports overflow, unreadable root entries or ambiguous FADT
selection. Rejected and missing entries have their own indexed outcomes; old
slot contents are not promoted to current evidence. A complete namespace
capture requires a valid DSDT, no omitted/unreadable entries and valid captured
SSDTs within those fixed limits. It does not establish AML semantics or correct
hardware operation.

The loader enables this extension only when all eighteen additional mapped
slots (`DSDT`, sixteen SSDTs and `SSOV`) have exactly their preallocated 16 KiB
extents. Older media continues to use the original basic capture API. Ordinary
boot still does not invoke this opt-in diagnostic path. The product image
builder and offline decoder supply the corresponding slots and report capture
completeness separately from hardware acceptance.

Run the original-reader and verbatim slot-guard controls with a new directory:

```sh
python3 tools/daemonarchy/tests/boottrace-acpi-namespace.py \
  --sanitize --cc /path/to/clang --work-dir /absolute/new/controls
```

Twenty-four cases pass with address and undefined-behavior sanitizers on Linux
and native FreeBSD ARM64. They cover legacy media, 32/64-bit table roots, FADT
length bounds, extended-pointer precedence/refusal, malformed DSDT/SSDT tables,
changing signatures, capacity overflow, missing/unreadable root entries and
every absent or incorrectly sized additional slot. Native selected source and
observed host/tool bytes remain guarded; bootstrap source provenance is not
implicitly admitted by these function controls.

The complete original native `stand` build also returns zero against the F4
source selection with these three loader-source overlays. Independent readback
validates all three actual AArch64 PE32+ EFI interpreters and the linked Lua
loader's namespace-reader symbol. All 115,681 original/candidate source entries,
7,172 selected source-built bootstrap nodes and observed host bytes pass their
preservation checks. A failed invocation and an incorrect post-build filename
observer remain retained separately from the successful native build.

The exact private image boots the new Lua loader and original matching kernel
in a generic ARM64 UEFI guest. Its actual DSDT validates, and all 29 capture,
index and state files match byte for byte on the returned disk after power-off.
This guest supplies no SSDTs; the multiple-SSDT positive cases remain covered
by the native production-reader controls. The VM powers off cleanly and its
frozen base remains unchanged. Complete native source/work/products and failed
attempts are archived and independently reopened against the selected inputs.

The [dated record](../verification/pi-acpi-namespace-20261122.json) binds these
scoped products and controls. Physical Pi namespace capture, SDIO/GPIO
integration, human authority review and canonical source/toolchain/lifecycle
admission remain separate gates. Existing operator images and the physical Pi
desktop remain unchanged while those checks run.
