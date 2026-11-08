# BCM2712 reset prerequisites for RP1

This isolated follow-on adds two reset providers required before an owned
BCM2712 PCIe/RP1 path can initialize its bridge. It starts from the qualified
SD/provider/ACPI source checkpoint `9252d5ebf72bf284bf0d481b492a101a1e90b6da`.
It does not replace that checkpoint's kernel or media. Host callback controls
are available; native compilation and physical reset/PCIe acceptance remain
separate gates. Attaching either provider performs no hardware writes.

## Source custody

The implementation in `sys/arm64/broadcom/bcmstb_reset.c` adapts these official
OpenBSD files and retains Mark Kettenis's 2025 ISC notice verbatim:

| Original source | CVS revision | Immutable Git blob |
| --- | --- | --- |
| [bcmstbreset.c](https://github.com/openbsd/src/blob/05f77a90390525396f20071b9d918f94fef30e02/sys/dev/fdt/bcmstbreset.c) | 1.1, 2025-08-20 | `7b3ce5aec1f7750c0786d379adf67edec8749b30` |
| [bcmstbrescal.c](https://github.com/openbsd/src/blob/05f77a90390525396f20071b9d918f94fef30e02/sys/dev/fdt/bcmstbrescal.c) | 1.1, 2025-08-20 | `9be54803e7597e54c960c102400cccf0ef1a67bb` |

Both blobs were independently checked against that exact OpenBSD repository
commit. The ISC notice permits copying and modification while requiring its
copyright and permission notice in all copies. The new work translates the
register contracts into FreeBSD's hwreset interfaces; it imports none of the
unattributed D590xx draft implementations.

The existing dual-licensed reset bindings describe the one-cell SW_INIT banks
and zero-cell RESCAL interface. Pinned Raspberry Pi Linux commit
`2569533d6c33c15231fbc21704c87fcebc67173c`, files
[`reset-brcmstb.c`](https://github.com/raspberrypi/linux/blob/2569533d6c33c15231fbc21704c87fcebc67173c/drivers/reset/reset-brcmstb.c)
and
[`reset-brcmstb-rescal.c`](https://github.com/raspberrypi/linux/blob/2569533d6c33c15231fbc21704c87fcebc67173c/drivers/reset/reset-brcmstb-rescal.c),
was checked for STATUS offsets, deassert settling and the DT pulse convention.
It is a hardware/API reference; Linux implementation code is not imported.

## Provider behavior

`brcm,brcmstb-reset` uses 32 reset lines per 24-byte bank. SET, CLEAR and STATUS
are at offsets 0, 4 and 8 within each bank. The selected Pi500 node at CPU
`0x1001504318` has 48 bytes; PCIe2's reset 44 is bank 1, bit 12. The provider checks
cell width, integral bank geometry, integer-ID bounds and xref identity before
MMIO. Assert/deassert writes only the selected bit to its SET/CLEAR register,
orders it, and polls actual STATUS with a 1 ms bound. Successful deassert also
waits 100 us for the downstream block. STATUS reads report the actual line.

`brcm,bcm7216-pcie-sata-rescal` at CPU `0x1000119500` is a self-deasserting
calibration pulse, with zero reset cells and ID 0. It cannot hold a persistent
reset line. FreeBSD's public hwreset API offers assert/deassert and state query,
but no separate pulse operation. The semantic translation is explicit:

- `hwreset_assert()` verifies that no request is in flight. It does not claim
  calibration or an asserted level, and performs no write when idle.
- `hwreset_deassert()` starts calibration, checks START readback, polls DONE
  with a 1 ms bound, clears START after completion and checks idle readback.
  It returns success only after that sequence completes.
- `hwreset_is_asserted()` returns `EOPNOTSUPP`; it does not invent a stable line
  state from the pulse engine's START bit.

This supports a DT consumer that only deasserts, and an assert/deassert pair
runs one pulse. It deliberately differs from OpenBSD's assert-trigger callback
because Linux's binding consumers trigger calibration on deassert. A busy START,
start/clear failure or timeout returns an error. Timeout preserves the in-flight
request; another call cannot silently retrigger it or claim it idle. Independent
physical recovery from a timed-out calibration is not implemented.

Both providers publish only after validated mapping and lock setup, at
`BUS_PASS_RESOURCE + BUS_PASS_ORDER_EARLY`. Published providers remain for the
kernel lifetime: hwreset consumer handles are raw provider references, not
Newbus child/busy references, so detach returns `EBUSY`. Failed unpublished
attachment releases its resource. If that release itself fails, it keeps an
unpublished, unavailable owner for explicit detach retry; no xref is published
and all operational callbacks reject it. Newbus consequently cannot free the
backing softc while the resource remains owned.

## Controls and build hooks

```
python3 tools/daemonarchy/tests/bcmstb-reset-controls.py --cc cc --sanitize
python3 tools/daemonarchy/tests/bcmstb-reset-controls.py --cc clang --sanitize
```

The harness compiles the complete production driver translation unit against
bounded kernel/OF/MMIO services. It exercises all 64 SW_INIT bits, reset 44,
non-owned status-bit preservation, bounds/cell/xref checks, actual status
polling and timeouts, paired and deassert-only RESCAL, stale DONE invalidation,
in-flight refusal, start/clear failures, publication order, allocation/geometry
failures and retained-resource cleanup retry. GCC and Clang ASan/UBSan controls
pass. These prove software control flow against that register model, not real
clock/reset hardware, elapsed calibration time or PCIe link training.

`files.arm64` includes the driver when FDT and hwreset are selected; the GENERIC
expanded configuration already selects hwreset through `std.dev`. The source
workflow watches the driver and runs the sanitized control. A clean native
GENERIC build with its source/object/configuration identities is still required.
No aggregate kernel build runs on the operator desktop.

## Remaining RP1 dependency order

The selected DTB has an AON GPIO2 `gpio-hog` requesting output-high for RP1 RUN.
The [separate selected-hog follow-on](pi5-aon-gpio-hogs.md) now supplies full
preflight, held reservations, private ordered application/readback and retained
unpublished owners on partial failure. Host controls do not establish BCM2712
pad state, reset timing or RP1 operation. This reset-provider delta alone does
not implement the hog.

Reset/rescal and GIC availability precede the missing BCM2712 MSI controller
and PCIe host support. MIP maps 64 MSI vectors to GIC SPI 128..191; its target
bus address `0xfffffff000` is not a CPU MMIO allocation. The PCIe host must
honor the selected reset providers, external MSI parent and DMA translations
before enumerating RP1. RP1 then needs a checked BAR/MSI-X nexus, clock provider
with the existing 50 MHz fixed xosc input, and GPIO/pinctrl before peripheral
consumers. Its Ethernet is Cadence GEM, not GENET.

Official ISC sources also exist for OpenBSD's `bcm2711_pcie.c` (including
BCM2712), `bcm2712_mip.c`, `rpone.c` and `rpiclock.c`. They are reviewable
references, not qualification: the retained FreeBSD drafts omit source notices
and contain concrete resource/polling defects, while even the original MIP
code needs a 64-bit allocation-bit audit before porting. Wholesale draft import
is outside this change. Initial on-SoC SD does not depend on RP1, and the
BCM2711 Pi4 simulator cannot qualify BCM2712 or RP1 hardware.
