# BCM2712 SDHCI bootstrap attachment

The BCM2712 FDT describes separate `host` and `cfg` register banks and a
`sw_sdio` clock. The initial generic compatibility match did not own those
board resources. The new BSD subclass retains the existing SDHCI command and
transfer engine while adding an explicit resource and power lifecycle for a
conservative Pi 5-family bootstrap profile.

The subclass layout follows the existing BSD `sdhci_fdt_xilinx.c` organization
and retains its attribution. Linux v6.12
[`sdhci-brcmstb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/mmc/host/sdhci-brcmstb.c)
and the dual-licensed
[`brcm,sdhci-brcmstb` binding](../sys/contrib/device-tree/Bindings/mmc/brcm,sdhci-brcmstb.yaml)
were behavioral references. No Linux implementation is copied into the BSD
driver. Source research and native compilation do not establish board support.

## Selected bootstrap behavior

- Require `brcm,bcm2712-sdhci`, canonical named `host`/`cfg` banks of sufficient
  size, the `sw_sdio` provider and a nonzero provider clock rate.
- Resolve declared default pinctrl, card-detect/write-protect GPIOs and
  optional supply providers. A declared but unresolved provider is an attach
  error; an unresolved declared card-detect GPIO is not replaced with an
  always present card. GPIO card detection uses polling and honors active-low flags
  plus the explicit FDT inversion property.
- Select the CFG maximum-50-MHz path with its strap override, preserve other
  register fields and check the mode readback. Non-removable/broken-card-detect
  profiles select the documented forced-presence polarity.
- Keep the enabled provider rate as the SDHCI base clock for divider math.
  The capability view clears the separate firmware-reported base-frequency
  field so the core cannot overwrite that rate. Limit the **card** clock and
  advertised maximum to 50 MHz or a lower FDT `max-frequency` value.
- Use 3.3 V power/signaling, retain standard legacy/high-speed modes and
  filter UHS, 1.8 V and tuned-mode capability bits. Honor the FDT bus width.
  No 1.8 V transition or tuning implementation is implied.

The Linux BCM2712-specific CFG initialization selects its delay-line path
when tuned modes are supported; this bootstrap deliberately selects the
other path and caps the card clock accordingly. Existing generic BSD SDHCI
already suppresses modes lacking timing/tuning methods. The new subclass
does not claim that the earlier generic match enabled unsupported tuning.

The enhanced optional `sdio_freq` rate-change profile and `mmc-pwrseq` are
explicitly refused until their own lifecycles are implemented. MMCCAM is also
refused at attach: its power/IOS path needs separate qualification rather than
silently omitting CAM initialization. These are bounded bootstrap limits,
not final Raspberry Pi distribution acceptance.

## Resource and failure lifecycle

The subclass stores the generic FDT softc first, preserving the inherited
register accessors and MMC methods. It owns a single initialized slot and
tracks whether initialization succeeded before calling the core cleanup.
Its interrupt handler is installed only after slot initialization and removed
before slot work is drained.

Detach first asks children to detach; a busy child leaves the controller and
its resources intact. Successful cleanup drains the slot, releases GPIO pins,
restores saved CFG fields and supply voltages, balances owned regulator and
clock enables, then releases resource handles. Power-off removes signal-rail
power before card-rail power. A failed signal-rail disable leaves the card
rail enabled and returns the failure. Failed power-up rolls back only enables
introduced by that operation. Provider and configuration failures identify
the failed attach stage.

These are software lifecycle rules. Native provider-failure injection,
actual Newbus detach/resume behavior, pre-attach pinmux restoration and
physical power sequencing remain unqualified. The pinctrl framework does not
provide a saved prior pinmux state for this attachment to restore.

## Validation and remaining gates

The small host control exercises the actual shared CFG/clock policy helpers:

```sh
cc -std=c17 -Wall -Wextra -Werror -I sys/dev/sdhci \
  tools/daemonarchy/tests/bcm2712-sdhci-policy.c -o /private/output/policy-test
/private/output/policy-test
```

It checks explicit register vectors, preservation of non-owned fields,
forced-presence polarity, rejection of incomplete 50-MHz-mode readbacks and
card-clock stop/identification/limit cases. It does not simulate the SDHCI
controller or prove the attach/unwind paths.

The native check uses a new source export and an **empty** GENERIC object
directory, regenerates configuration and builds/links the kernel. Compiler,
changed-object, linked-kernel and extracted-configuration identities must be
retained with the accepted build. A compiler exit or an old aggregate object
tree alone is not selected-source evidence. Framework per-unit admission
and native hardware acceptance remain separate gates.

The accepted native check completed on FreeBSD 16-CURRENT ARM64 with FreeBSD
clang 21.1.8. It exported source base
`b417aa10e466440c5c166f4e0b2ff4dcb52e32fd` plus the reviewed glue files,
configured an empty GENERIC object directory, and completed
`make -j6 kernel.full` followed by `make kernel`. The log includes the new
driver compilation and the stripped kernel contains its controller
description. The policy controls also passed natively and on Linux ARM64.

| Accepted native input/output | SHA-256 |
| --- | --- |
| `sdhci_fdt_bcm2712.c` | `c58d85d7c0a186f21d88ae45a7fafde36f11f547545b03df3bf4fc08e83ba12b` |
| `sdhci_bcm2712_policy.h` | `53a18434e50be45e59ab28c97ae3f5e46e49e3fea6ca62fe20fa9ca3cacb7662` |
| New driver object | `5a2c1813b6ef9b88a8cc51a12fa301bde928f70fe81ecf00ed18bd97cde10b6a` |
| Linked `kernel.full` | `e351d4a41346fefd73bb3f9e8c67bc097c3ec8504abf59a5c868ea8e0aa64537` |
| Stripped boot `kernel` | `79ebe4bfcb5f46bbe61c4611d0875beb876b6a523f5b7b187f3c49b0ee463eeb` |
| Extracted GENERIC configuration | `be073065fb6024d3a5d9bd130b1d920ef03a6b7df635d01154cfee0bb26b1fda` |

This is a conventional clean native build with the existing worker toolchain,
not a new claim of compiler, per-unit packaging or Standard receipt admission.

The Pi 4/CM4 simulator does not model BCM2712's host/CFG banks, pinctrl, supply
providers or GPIO wiring. A Pi 4 boot can check a new kernel's basic artifact
and world compatibility, but cannot qualify this controller. The physical
boards are unattended and were not rebooted or reconfigured for this work.
Native SD transfers, removal/power/reset recovery, full-RAM DMA, tuned modes,
RP1 peripherals and installed distribution acceptance remain open.
