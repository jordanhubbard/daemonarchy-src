# NVIDIA RTX Spark bring-up: findings and plan (draft, 1.5.1)

Status: **draft analysis, nothing here has run on the Spark.** The kernel change
in this branch was built natively on the arm64 worker and booted under
QEMU/TCG only (4 vCPUs, fault injection). It does not claim to fix the hang. It
turns the hang and the later panic into a boot on the CPUs that did start and
records why the others did not. Read with
`docs/application-note-rtx-spark.md` and `docs/aarch64.md` in the
`daemonarchy` repository. The hwtrace queues are in
[queue 6a](rtx-spark-queue6a.txt) and [queue 6b](rtx-spark-queue6b.txt) here.

## 1. The reference repository

`pfeifferj/archlinuxarm-proart-p16` (MIT licence, Copyright (c) 2026 Josephine
Pfeiffer) was cloned into an empty scratch directory and **read only**: no
script, package or binary from it was executed or installed, and no code or
data was copied; facts below were re-derived from reading it.

It is an Arch Linux ARM installer for the **ASUS ProArt P16 H7607BA** (firmware
H7607BA.300), an NVIDIA N1X laptop with MediaTek-built peripherals (MT8901
audio, MT7925 Wi-Fi/Bluetooth). It is not literally an "RTX Spark" and it is
tested on one machine; that it shares the SoC family with the Spark is the
operator's statement and is not verified here. This matters because **the
repository contains nothing about the failure we have**: no PSCI, GIC, SMMU or
CPU-start material at all. Linux simply starts all 20 cores.

What it does establish:

| Area | Reference (Linux) | FreeBSD tree today |
| --- | --- | --- |
| Boot | UEFI, ACPI only (no device tree), systemd-boot, EDK2 shell, then Linux with an early ACPI override archive | UEFI loader, ACPI, efifb. No override mechanism |
| Kernel | NVIDIA NV-Kernels `2234cd7f` (Ubuntu `linux-nvidia-7.0` 7.0.0-1022), ACPI-override allocation patch | FreeBSD 16-CURRENT GENERIC |
| CPUs | `ARM_PSCI_FW`, ACPI PPTT, ACPI parking protocol enabled, CPPC cpufreq, SCMI | PSCI via ACPI; no cpufreq driver for this SoC |
| Interrupts | GICv3 + ITS | `gic_v3`, `gicv3_its` present |
| IOMMU | `ARM_SMMU_V3` | `options IOMMU` is **not** in GENERIC, so no SMMU driver is compiled |
| PCIe | ECAM through ACPI MCFG; `PCIE_MEDIATEK_GEN3` is a module | `pci_host_generic_acpi` present; no MediaTek Gen3 host driver |
| USB | `xhci_pci`, `xhci_mtk`, `i2c_mt65xx`, `i2c_hid_acpi` | xhci works (queue 4 trace 0002 shows hubs and umass); no MediaTek-specific parts |
| GPU | out-of-tree `nvidia-open` 615.78.08 with a Tegra RM API library, a DRM Normal-NC framebuffer patch and `simpledrm` blacklisted | no driver; efifb works (3840x2400 seen) |
| Wi-Fi | `mt7925e` with `linux-firmware-mediatek` | `mt76` LinuxKPI driver **including mt7925** is in `sys/contrib/dev/mediatek`; ports already carry `wifi-firmware-mt76-kmod@mt7925` |
| Bluetooth | `btmtk` | MediaTek USB Bluetooth (seen as "MediaTek Inc. WirelessDevice") has no FreeBSD firmware loader: unsupported |
| Audio | MT8901 SoundWire machine driver + Cirrus CS35L56 firmware | no driver: unsupported |
| Power | CPPC range restore script, `power-profiles-daemon` | not applicable |

Linux command line used by the reference: `rw rootwait console=tty0
acpi=nospcr thermal.tzp=100 mem_sleep_default=s2idle
initcall_blacklist=simpledrm_platform_driver_init fbcon=font:TER16x32
log_buf_len=4M loglevel=3`. Nothing in it is a CPU-start workaround. The FreeBSD
analogues are: `acpi=nospcr` is no console selection from SPCR (FreeBSD uses
efifb; keep `boot_serial` off), `simpledrm` is not relevant, `s2idle` is
`hw.acpi.lid_switch_state`/suspend policy and is out of scope until the machine
boots.

Firmware-specific ACPI work in the reference is for the P16's own tables:
replacement DSDT and SSDT built from the user's own dump (the repository states
the ASUS and NVIDIA tables may not be redistributed), a patch of
`\_SB.SSPM._STA` (device `NVDA8400`) so that it does not depend on
`_OSI("Windows 2022")` (receipt marks it `hardware_boot_verified: false`), six
embedded-controller and battery methods, and a kernel patch for an override
table straddling an EFI memory descriptor. None of it applies to FreeBSD, which
loads no overrides. FreeBSD's ACPICA answers `_OSI("Windows 2022")` true by
default, as Linux does, so the `_STA` question is worth a look in the DSDT the
hwtrace image dumps, but nothing suggests it affects CPU start.

## 2. What the traces say

* The backed-up stick (`~/spark-queue5-stick-backup`) does **not** contain queue-5
  results. `STATE.TXT` reads `next=1 traces=2 pending=` and `QUEUE/` holds the
  queue-5 entries written on Nov 3 07:33: queue 5 had not run when the backup
  was taken. The two traces are from queue 4 (patched kernel `#0`, Nov 2 11:12
  PST). `DUMPS/0002` and `DUMPS/0003` are empty (the machine was powered off
  before the stick flushed).
* Trace 0001, `patched-stock` (all CPUs): **no NVRAM trace.** The kernel never
  reached EFI runtime services. The operator saw a kernel panic on screen; its
  text is not recorded anywhere on the stick.
* Trace 0002, `hw.ncpu=4` (CPUs 0-3 only): `complete: the kernel reached init`
  at 2.794 s, 29,779 steps. USB hubs, umass, a USB Flash Disk and a
  "MediaTek Inc. WirelessDevice" on usbus4 attach. So four CPUs and the
  patched kernel work; the problem begins with the fifth CPU.
* The photo of the stock boot (`IMG_2221`): ACPI reports 20 CPUs, 15,274 MB RAM,
  `FreeBSD 16.0-CURRENT #2 303b3646270f`, clang 21.1.0, WITNESS on, and the
  console ends at `Starting CPU 4 (400)`.

**Why the panic text cannot be filed:** `dbt_panic()` writes the NVRAM trace
only when `dbt_rt_ready` (EFI runtime services usable). CPU start happens at
`SI_SUB_CPU`, long before that, so any panic during SMP start leaves no trace.
The first line of the screen is the only record. Queue 6 therefore keeps the
panic on screen for 60 s (`kern.panic_reboot_wait_time=60`) and asks for a
photo.

**CPU numbering.** The other Spark-class machine in the lab (the DGX Spark
`sparky`, GB10, 20 CPUs) lists under Linux: CPUs 0-4 Cortex-A725, 5-9
Cortex-X925, 10-14 A725, 15-19 X925, all r0p1, all with the same MIDR within a
core type. If the laptop orders cores the same way (unverified), MPIDR 0x400 is
the fifth and last A725 of the first group, with the same MIDR as the three that
started, and CPU 5 would be the first X925. A core-type difference therefore does
not by itself explain why 0x400 is the first to fail.

## 3. The CPU 4 (0x400) hang: what is and is not known

`start_cpu()` calls PSCI CPU_ON, which returned success, and then waits for
`aps_started` to change. The first increment is in `init_secondary()` after
`ptrauth_mp_start()`, `identify_cpu()` and `enable_cpu_feat(CPU_FEAT_EARLY_BOOT)`.
So the failure is somewhere between the PSCI entry and that increment. The
cause is **not established**. Candidates, with no hardware evidence yet for any:

1. The core never reaches `mpentry_common` (firmware power-up of that core, an
   SSPM/mailbox dependency or a PSCI quirk). Stage 0 in the new report.
2. The core runs but dies with the MMU off or at the first kernel exception
   level before vectors exist (silent). Stage 1 or 2.
3. The core reaches the kernel and faults in `identify_cpu()` or the early
   feature enable (an exception before the AP has a handler hangs silently).
   Stage 3 or 4.
4. The core is merely very slow (firmware power-up taking longer than anyone
   waited). Reported as "checked in after N ms" with a 20 s timeout.

Why PR #53 then panics (reading code, not observed): after a timeout the boot
CPU hands the next CPU the *same* `bootstack`/`bootpcpu` globals. A core that
turns out to be late (candidate 4) would run on another AP's stack. The
timeout path also keeps going with a hole in the CPU ID space. The panic text
is needed to say more; the branch makes the late-arrival case harmless
(parked in `locore.S` before it touches the stack, the pcpu or any counter) by
default.

**An attempt that failed and is worth knowing:** lowering `mp_maxid` after the
failed CPUs to remove the hole panicked immediately under QEMU (a UMA zone
lock was used before initialization). `mp_maxid` is deliberately left alone.

## 4. Changes ranked by confidence

Confidence is that the change helps the Spark, not that it is correct code.

| # | Change | Where | Confidence | State |
| --- | --- | --- | --- | --- |
| 1 | Stop starting CPUs after the first one that fails; park any late arrival; report early stage 1/2 (MMU-off stores), stage 3-5, PSCI AFFINITY_INFO and per-CPU start time; 5 s default timeout | this branch | High that it reaches init on four CPUs (matches the known-good `hw.ncpu=4`); high that it names the stage | built on arm64, QEMU TCG only |
| 2 | Tunables `hw.mp.ap_timeout_ms`, `hw.mp.stop_on_failure`, `hw.mp.skip_mpidr`, `hw.mp.fail_mpidr` (fault injection) so experiments need no new kernel | this branch | High (diagnostic) | QEMU TCG only |
| 3 | Photo-friendly panic hold in the queue | queue 6 | High | not yet run |
| 4 | The actual CPU-start fix | unknown | Low until queue 6a returns | needs hardware |
| 5 | If the stage is 0 or 2 for 0x400 only: skip that core by default for this SoC (`hw.mp.skip_mpidr` equivalent in a quirk table) and look at SSPM `NVDA8400` in the dumped DSDT | later | Low-medium | needs dumps |
| 6 | `options IOMMU` and the SMMU driver in the aarch64 kernel (the Linux build uses SMMUv3) | later | Low: only matters if DMA from NVMe/Wi-Fi fails once SMP works | needs `pciconf`/`dmesg` from a boot |
| 7 | Wi-Fi firmware | none | The mt7925 driver and firmware package already exist: nothing for the ports tree. Bluetooth, audio, GPU have no FreeBSD driver and are out of scope | n/a |

No `daemonarchy-ports` change is proposed: nothing found needs a new package.

## 5. Hands-off hwtrace plan

Needs from the operator: one stick to be **overwritten** with the image below
(the sticks currently hold the amd64 ISO; this plan never writes a device and
the image was not written anywhere), and a photo of the screen if a panic
occurs. The image is the existing aarch64 hwtrace image with the new kernel
swapped in and queue 6a staged; it is an unsigned developer image, not a
release asset.

Queue 6a (`[diag-stop-20s]`, then `[diag-skip-400]`, then `[ncpu-4]`) runs
until the first boot that reaches init, which saves `dmesg`, `sysctl`,
`pciconf`, `devinfo` and `acpidump` to `DUMPS/<n>` and holds 120 s. What to look
for in the dump's `dmesg`: `CPU 4 (400) checked in after N ms` (slow core) or
`failed to start after N ms (early stage S, stage T, PSCI AFFINITY_INFO A)`.
Queue 6b then asks whether only core 0x400 is bad and whether continuing past a
failed core really panics. Do not pull power or the stick during the hold.
