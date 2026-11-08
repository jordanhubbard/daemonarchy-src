# Native translated DMA diagnostic fixture

This fixture exercises the production ARM64 translated busdma backend on the
existing BCM2711 simulator. It does not attach a BCM2712 or RP1 device, qualify
physical hardware, add emulator features, or test MSI delivery. Normal GENERIC
behavior is unchanged when `BCM2711_DMA_TEST` is absent or its boot tunable is
not 1. Loading the probe module performs no automatic disk writes.

The corrected kernel source is `32a3c52a1b5f47e55ae009991e80b6a4251dc635`, with
backend implementation `4179e2cee4aeea0f902fd182ba48a35cc11fc2e8` and its reviewed
head `b2f5ec95080947df7a18f33861cc0e059530bc2c` merged. The probe module source is
`e5002283c874d402fd5889ec988fc0d3d7112250`. Both artifacts compiled successfully
on the allocated FreeBSD ARM64 worker against the same generated kernel headers.

| Artifact | SHA256 |
| --- | --- |
| `BCM2711_DMA_TEST` kernel | `da51c55c69c3ab171d391cf49e6273de8d120885cbbaf5097984888825b55629` |
| `bcm2711_dma_probe.ko` | `e313842268d53a9194c3f20c9fe267dda843b69480602bdf12e59c8aa01e3342` |
| Existing, unchanged QEMU | `08d5088766b2670847f0ec1c57cc7aeceb24f6c3fc98caff6ed2f6265718d834` |

Compilation and controller tests do not establish native DMA execution. A cold
VM run, runtime pressure and error checks, the parent boot/network/reset receipt,
and independent offline USB payload verification are separate acceptance gates.
Physical Pi testing and BCM2712/RP1 attachment remain separate work.

## Enrollment and observations

Build `sys/arm64/conf/BCM2711_DMA_TEST` and explicitly set
`hw.bcm2711_dma_test.enable="1"`. Diagnostic enrollment requires both
`brcm,bcm2711-pcie` and the existing QEMU root port's actual ID, `000c:1b36`.
This is a guard for the simulator, not a new physical device compatibility.

The BCM host prepares the production translated tag under the existing 960 MiB
physical parent. It performs normal identity reset and link initialization,
checks the QEMU ID after link-up, then programs BAR2 HIGH=`0x10`/LOW=`0x12` and
verifies readback before publishing PCI children. PCI 64 GiB maps to CPU physical
0 through the model's existing 8 GiB RAM window. The selected 64 GiB policy is an
address aperture, not a claim of 64 GiB physical RAM. The attached host retains
both tag owners until guest reset.

Normal PCI, xHCI, USB and umass inherit the production implementation through
ordinary child-tag creation. Diagnostic observers do not alter returned
addresses or completion status. They record actual programmed DCBAAP, ERSTBA
and CRCR values, plus the actual USB busdma load callback's KVA and segments.
A bounded bitmap requires complete coverage of the original 128 KiB buffer in
512-byte units; duplicate callbacks cannot substitute for missing ranges.
Returned device addresses must carry the 64 GiB offset and remain within the
physical parent's prefix.

The umass `proxy_buffer` flag does not substitute a low allocation.
`usb_transfer.c` uses it to round transfer size; `ext_buffer=1` bypasses local
buffer allocation. Mapped GEOM BIO data passes through scsi_da's
`ccb->csio.data_ptr`, umass's transfer pointer and `usbd_xfer_set_frame_data`
to `usb_pc_load_mem`. Source review establishes that path; runtime coverage must
still prove the original high KVA reached the actual DMA callback. No custom
USB transport or proxy-off patch replaces normal umass.

## Native probe controls

Build on the allocated FreeBSD worker using a separate object directory and the
same generated headers as the test kernel. Verify `.OBJDIR` before compiling:

```sh
module_source=/absolute/frozen-src/sys/modules/bcm2711_dma_probe
module_obj=/absolute/module-obj
sys_source=/absolute/frozen-src/sys
kernel_obj=/absolute/frozen-kernel-obj
mkdir -p "$module_obj"
test "$(env MAKEOBJDIR="$module_obj" make -C "$module_source" \
  SYSDIR="$sys_source" KERNBUILDDIR="$kernel_obj" -V .OBJDIR)" = "$module_obj"
env MAKEOBJDIR="$module_obj" make -C "$module_source" \
  SYSDIR="$sys_source" KERNBUILDDIR="$kernel_obj" DEBUG_FLAGS=-g all
```

Use native SD UFS root, all 4 CPUs and the full 8 GiB board, with GENET over
restricted user networking forwarded only to loopback. Attach one fresh,
unformatted, raw 16 MiB USB scratch disk as QEMU backend `dmadisk` under
`bcm-xhci.0`. The probe accepts only raw GEOM DISK `da0` at that exact size with
512-byte sectors and obtains exclusive access. Only this disposable disk is
used for USB writes.

Stage the module under volatile `/var/tmp`; `/tmp` is read-only in the pinned
minimal guest. That guest has no `sha256` command, `vmstat`, or `kldunload`
symlink. The controller uses trusted SCP readbacks for host SHA256 custody,
raw interrupt sysctls, and the applicable `/rescue/rescue` applets.

| `debug.bcm2711_dma_probe.run` | Actual operation |
| --- | --- |
| 1 | Verify physical source pages in [4 GiB, 8 GiB); run eight 128 KiB GEOM WRITE/READ pairs with original high KVA, exact callback coverage, SHA256 and canaries |
| 2 | Prepare bounded real maps under an owned 512 MiB physical prefix with page alignment; retain owners for external zone observation |
| 3 | Reap retained owners only after the actual driver mutex and unlock prove callback completion; queued state remains busy |
| 4 | Consume real pool pages; require NOWAIT ENOMEM, WAITOK EINPROGRESS and queued destroy EBUSY; release holders and verify exactly one callback that unloads and destroys its map/tag, followed by the saved worker unlock |
| 5 | Prepare a tag with a maximum size of 2 pages, one segment and a maximum segment size of one page, plus a high source buffer of 3 pages; permit external baseline observation |
| 6 | Require segment-budget and overmax EFBIG errors through exactly one callback with zero segments, then verify one-page load/sync recovery on the same map and clean up owners |

Completed EFBIG is an MI callback result: `bus_dmamap_load` itself returns 0 for
that completed error. ENOMEM and EINPROGRESS have distinct return contracts.
The fixture follows the actual MI implementation.

Before requesting 4, the controller refuses any existing compatible stricter
bounce zone, then observes the newly created zone and its actual bounded/free
page counts. It checks exactly one deferral and full page return. It snapshots
the same isolated pool after 5 and requires unchanged free, reserved and active
page accounting after 6. No allocator, busdma thread or callback is replaced,
and the fixture does not exhaust memory globally.

Queued owners must refuse destruction. Callback-time destruction is allowed by
the existing API: the worker's cached unlock function and argument must survive.
The heap lock owner remains alive until the real unlock completes. Timeouts
retain owners and block module unload. Source buffers and GEOM consumers are
also retained if anomalous cleanup cannot complete.

The controller reads `hw.intrnames` and `hw.intrcnt` as binary sysctls into
`/var/tmp` and copies them to the host. It parses ARM64 little-endian 64-bit
counters and individual fixed slots of `2 * MAXCOMLEN + 1` bytes, including unused
slots containing only NUL bytes. It requires progress in the normal `xhci0`
counter and records the paired `stray ` counter separately.

## Parent lifecycle and controller phases

`tools/daemonarchy/tests/bcm2711-dma-native-control.py` provides three phases.
The existing private NetworkControl parent owns image enrollment, VM startup,
trusted SSH, diagnostics, continuation and natural reset. The phase controller
never stops or continues a VM and never changes its boot configuration.

```sh
python3 tools/daemonarchy/tests/bcm2711-dma-native-control.py prepare \
  --source-image /path/to/enrolled/control.img --output /fresh/native-evidence
# Parent attaches /fresh/native-evidence/scratch.raw as dmadisk and boots.
python3 tools/daemonarchy/tests/bcm2711-dma-native-control.py run \
  --output /fresh/native-evidence --qemu /path/to/qualified/qemu-system-aarch64 \
  --kernel /path/to/kernel --module /path/to/bcm2711_dma_probe.ko \
  --qmp /path/to/vm/qmp.sock --pid PID --ssh-port PORT \
  --identity /private/client-key --known-hosts /private/known-hosts \
  --sd-overlay /path/to/vm/disk.qcow2
# Only after the parent accepts boot, network, diagnostics and natural reset:
python3 tools/daemonarchy/tests/bcm2711-dma-native-control.py verify \
  --output /fresh/native-evidence
```

The enrolled `control.img` differs from the original source image after adding
VM keys and queue entries. Prepare must run after enrollment. The parent
separately pins and preserves the original source image.

Runtime checks bind the live QEMU process, kernel and module, actual SD GEOM
ancestry even when root uses `gpt/HWTROOT`, and exactly two disk backends with
the correct SD backing and USB attachment. They require GENET carrier/IP,
translated xHCI ring addresses, disabled MSI/MSI-X, native interrupt progress
and storage logs without errors. The order is high BIO, observed pressure,
size/segment errors, and high BIO recovery. SSH/SCP ignore ambient config and
agents and pin the VM host key. Guest kernel/module bytes are read back over
that connection and hashed on the host.

After the parent accepts exit and diagnostics, offline verification requires
the QEMU process to have ended, verifies the enrolled SD backing is unchanged,
reconstructs the fresh kernel nonce independently, checks every USB payload
byte, and checks that scratch bytes outside the test range remain zero. A
runtime-only pass does not satisfy the parent lifecycle gate.

Run the eighteen controller/refusal/offline-data controls with:

```sh
python3 tools/daemonarchy/tests/test-bcm2711-dma-native-control.py
```

These validate the fixture's guards and oracle. They do not replace a native
backend experiment or physical hardware testing.

## Retained failures and current status

The first native experiment refused readiness before loading the probe or doing
USB test I/O. Its initial ID check preceded cold link enable, so the model
returned all ones for unavailable config. QMP independently showed the expected
QEMU ID. The hook-only correction above preserves normal identity initialization
and checks config after link-up, before writing a special translated window.
The model, backend and probe module were not changed to hide that failure.

The next experiment completed real high-buffer I/O, pool pressure, callback
ownership, EFBIG checks and recovery. Its phase controller then failed because
`vmstat` was absent. Partial offline nonce and pool evidence was retained, but
the parent stopped without claiming complete acceptance. A subsequent raw-counter run measured normal IRQ progress
from 232 to 660 and
zero strays, but rejected the kernel's retained bare stray label. The measured
pair is now a sealed regression fixture. The final parser accepts the same IRQ
source's base stray label or its complete driver suffix, while preserving slot,
name and counter checks. Source review also corrected unload to use the exact
hashed linker filename that was loaded, rather than the moduledata name.

The fresh complete experiment passed with these unchanged C products. All 27
parent guards passed, all seven native probes returned status 0, and all 15
durable diagnostic files were verified. The full 8 GiB profile reported
8,429,989,888 physical bytes, native SD root, GENET transfers and natural guest
reset. Two runs of eight 128 KiB READ/WRITE pairs completed with both source
buffers above 4 GiB and complete high-KVA callback coverage. Genuine pool
pressure, callback-time teardown, both EFBIG cases, sync recovery and exact
module unload passed. The normal xHCI counter increased by 428; the paired stray
counter stayed at zero. Independent offline verification checked every final
USB nonce byte, unchanged bytes outside the test range and unchanged SD input.

The scoped acceptance receipt is
`/home/jkh/daemonarchy-debug/bcm2711-translated-dma-native-20261108-r2/accepted-runtime.json`,
SHA256 `fa5c7526c4e83de3f734781f9354ed160ed4aeb0d7c820f7b4a8daaff3e1de97`.
Earlier failed and partial runs remain preserved. This qualifies native
translated RAM DMA and ownership for this simulator profile. Physical BCM2712/
RP1 controller, MSI delivery, reset and electrical cache-coherency gates remain
open.
