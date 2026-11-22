# Bounded SDIO initialization in MMC CAM

`MMCCAM-SDIO-001` fixes the CMD5 handshake in `sys/cam/mmc/mmc_xpt.c`.
An argument-zero query discovers the card's I/O functions and voltage windows;
it does not publish an initialized SDIO device. The next command requests only
voltage windows shared with the host. The selected-voltage response must report
ready before the probe exposes I/O functions or advances to RCA selection.
Combo cards continue through the existing memory initialization path.

This follows the [SD Association SDIO simplified specification, version 3.00,
sections 3.1, 3.3 and 3.4](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=PartE1_SDIO_Simplified_Specification_Ver3.00.pdf).
R4 has a three-bit function count (zero through seven); its voltage windows
occupy bits 8 through 23. A memory-only card can answer CMD5 without I/O
functions. That response, invalid or incompatible voltage windows, command or
transport errors, and exhausted initialization all preserve the memory probe
path while clearing provisional I/O metadata.

The first selected-voltage request starts a one-second elapsed-time budget.
An unsuccessful readiness response permits another request only while that
budget and the 1,000-request ceiling remain available. Each command also has a
one-second transport timeout. Retries use a one-millisecond delay in the
non-sleepable completion context. Queue latency and an in-flight command can
extend total elapsed probing time; this is not a hard real-time deadline.
A ready response can complete the handshake even if it arrives after the
readiness polling budget. A restarted probe clears its old I/O state and budget.

Run the focused controls with an explicit compiler and a fresh output directory:

```sh
python3 tools/daemonarchy/test_mmccam_sdio_init.py \
  --cc clang --sanitize --output /path/to/new-sdio-control
```

The test compiles verbatim production sender/completion cases, the restart
helper and the register definitions. CAM services, time, delay and structure
layouts are explicit userspace stubs. Enabled debug argument evaluation checks
that action logging cannot run the completion helper twice. Independent cases
exercise delayed readiness, all seven function counts, voltage selection,
ordinary SD and combo behavior, malformed replies, transport errors, retry and
elapsed limits, timer wrap and restart cleanup.

The retained original source can be selected with `--source`. It must compile
and report failed expectations to establish a regression control; compiler
failures, signals or sanitizer failures do not count as expected assertion
failures. The first retained baseline attempt exposed a harness trap after the
old code advanced early; the repaired harness retains that failure separately.

[Development evidence](../verification/mmccam-sdio-init-20261122.json) records
the exact inputs and completed controls. All 36 candidate expectations pass with
native FreeBSD ARM64 and AMD64 compilers and with Linux Clang 18 address and
undefined-behavior sanitizers. The original code compiles and fails 28 of the 35
shared expectations; the candidate has one additional restart-helper check.
Generated excerpts retain all three original files' copyright and license notices.
They also consume the original `CARD_FEATURE_*` definitions. Review exposed an
unparenthesized legacy shift in those definitions: negating the bare macro
cleared unrelated feature bits. Both clear operations now group the complete
macro expression. The first fixture had supplied a parenthesized definition;
that fixture and the first native kernel are retained as superseded evidence.
The corrected fixture rejects that first candidate at compilation with warnings
treated as errors, while the corrected source passes all 36 expectations.

A full native ARM64 `GENERIC-MMCCAM` build passes on retained F4 source with
exactly the two changed production files overlaid. Before/after checks preserve
all 115,681 entries in both original and private source trees, the original
1,113-node source-built bootstrap and the observed host files. This is a scoped
native build, not a complete build of the source review head or SDK admission.
Selected kernel/debug/module products and complete native evidence have been
archived and independently reopened on two physical filesystems. Full private
native work remains on its allocated worker.

The first native candidate boots in a generic ARM64 UEFI VM and exposes a QEMU SDHCI memory
card as `sdda0`. Its actual CMD5 trace takes the memory fallback. With default
DMA, the first large read fails with an SDHCI controller timeout; the prior F4
MMCCAM kernel reproduces that failure, while a single-sector read passes. The
prior kernel also contains three mailbox overlays, for which QEMU exposes no
matching device; this is not a two-file-only kernel counterfactual.
`MMCCAM-SDMA-001` tracks the unresolved DMA/emulation investigation.
Both kernels also emit a WITNESS allocation warning during locked discovery,
before CMD5. `MMCCAM-DISCOVERY-001` tracks that separate existing path. A boot
and functional data result do not qualify these unresolved diagnostics.

A separate fixture selects `SDHCI_QUIRK_BROKEN_DMA` (4) and confirms PIO in
the actual slot capabilities. Its first 64 MiB read made progress but exceeded
the capture's 60-second deadline; that attempt earns no completion or throughput
pass. The longer functional capture completed both 64 MiB readbacks and an
independent post-shutdown check for that first candidate. Its first piped write
was short and failed the byte expectation; `iflag=fullblock` completed the full
1 MiB write. These retained results do not qualify the later source correction.

The corrected kernel has its own fresh VM result: actual CMD5 memory fallback,
a 1 MiB pre-write read, an exactly 1 MiB write and matching readback. After actual
guest process exit, an independent comparison of the entire 64 MiB backing file
confirms that all bytes outside that write remain unchanged. Both the running
version's private build identity and the kernel-file hash bind this result to the
corrected native product. Production DMA policy is unchanged.

These controls do not establish physical controller or radio behavior.
Physical SDIO enumeration, firmware loading, Wi-Fi traffic and default DMA
operation remain distinct requirements. Bus-width changes, higher clocks and
1.8 V switching are outside this change. Physical Pi native SD and Ethernet
coverage, the desktop and other Broadcom devices remain in the release scope.
