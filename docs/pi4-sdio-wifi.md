# MMCCAM and Broadcom SDIO compatibility controls

Pi Wi-Fi needs SDIO discovery and the board's firmware, power and pin services
before a radio driver can attach. The original `GENERIC-MMCCAM` profile selects
the CAM MMC/SDIO stack instead of the legacy MMC stack. This development control
builds that existing profile with the [BCM2711 ACPI mailbox candidate](pi4-acpi-mailbox.md).
It does not change the default product kernel profile.

## Observed native build and VM behavior

The complete native ARM64 kernel build passes against the recorded F4 selection.
All 115,681 original/candidate source entries and the original bootstrap/host
pass before/after guards. The actual built kernel boots from a newly assembled,
independently read-back diagnostic image in a generic ARM64 UEFI VM with 1 GiB
of RAM. Its running kernel identity and kernel-file hash match the selected
product. The VM does not emulate a Pi SDHCI controller or Broadcom radio.

The original native Broadcom driver files match all 55 files and executable
modes in the checksum-pinned upstream archive at
`9adbe774d3be91827d628fbd811daf63512acb6d`. Source, Makefile, specifications,
documentation and ISC license remain retained. No Git commands or source edits
were made in the driver checkout. The existing owned port's nine downstream
patches were not part of this raw-driver control; their selected ARM build and
package acceptance remain required.

The subsequent patched-package control is recorded in ports
[main review #87](https://github.com/jordanhubbard/daemonarchy-ports/pull/87)
and [1.6 candidate #88](https://github.com/jordanhubbard/daemonarchy-ports/pull/88).
It preserves all nine patches and completes native arm64/amd64 packaging plus
independent install/delete/reinstall and two installed-driver load/unload cycles.
Its CTF-preserving build removes the missing-CTF warning observed below for the
raw driver. This does not qualify physical radio operation or full DTrace use.
The amd64 base SDIO dependency refuses unload by design of its current
`EOPNOTSUPP` deinitialization; a same-disk reboot restores baseline modules.
Exact products, source/kernel inputs, complete archives and remaining gates are
in that ports branch's `verification/brcmfmac-native-packages-20261122.json`.
The following raw-driver evidence and its original limitations are historical
controls retained unchanged.

The unmodified driver first compiles against the original F4 headers, then
against the exact completed MMCCAM kernel configuration, including 303 generated
headers. All 55 driver files, 36,893 kernel source/header entries, generated
headers, the selected kernel, observed tools and installed make rules pass their
before/after guards. The kernel-option-bound module differs from the standalone
compile, so the VM control uses only the option-bound product.

The actual kernel supplies SDIO, firmware and crypto symbols. Matching WLAN and
WPA2-CCMP modules supply the remaining imports; the symbol review finds no missing
imports. No duplicate SDIO, CAM, firmware or crypto module is loaded. In the VM,
the actual driver registers both PCI and SDIO frontends and passes two idle
load/unload cycles. Unloading the selected WLAN/CCMP modules restores the initial
kernel-file registry and network interface list. The owned VM then powers off
cleanly, with actual process exit and terminal service success observed.

## Remaining evidence limits

The standalone original module emits missing-CTF warnings during loading.
Kernel type information and DTrace qualification remain open; successful module
registration does not establish those debugging features.

The first module staging observer reported a byte disagreement before preserving
its raw readback. Three subsequent reads of the same file and all newly captured
module readbacks match. Nine further 4 MiB readbacks match three distinct binary
fixtures. The initial disagreement remains unclassified; no transport or kernel
integrity repair is inferred. Failed controls, original products, raw renewed
readbacks and the guest runtime archive remain privately retained.

The [scoped record](../verification/pi4-mmccam-brcmfmac-20261122.json) binds the
exact kernel, driver, source archive and fixture image. It is not a package
consumer or physical Wi-Fi result. The private image retains diagnostic
credentials and firmware files for the lab workflow and is not a new operator
Pi image or a release artifact.

Physical mailbox access, GPIO/power/pin sequencing, ACPI SDIO discovery, firmware
and board NVRAM/CLM selection, association, traffic and recovery remain open.
Native SD and Ethernet must be requalified with the MMCCAM selection before
product use. The owned port's downstream patches, actual ARM package consumers,
source/toolchain closure, human authority review and canonical lifecycle/release
acceptance remain separate gates.
