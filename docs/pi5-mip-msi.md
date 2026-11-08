# Selected BCM2712 MIP0 MSI provider

This isolated follow-on starts from GPIO ownership checkpoint
`07cca79102c3a9030469b8f33b3a6a45301f6c44`. It supplies the selected MIP0
MSI provider and a checked GICv2 range-reservation prerequisite. Host production
controls, a fresh native dependency-closure build and renewed Pi4 compatibility
controls pass. BCM2712/MSI delivery, PCIe and RP1 hardware operation remain
unqualified. No physical boot state or media was changed.

## Maintained source and selected binding

`sys/arm64/broadcom/bcm2712_mip.c` adapts Mark Kettenis's OpenBSD
[`bcm2712_mip.c`](https://github.com/openbsd/src/blob/05f77a90390525396f20071b9d918f94fef30e02/sys/arch/arm64/dev/bcm2712_mip.c),
CVS revision 1.1, 2025-08-21, immutable Git blob
`8d099d6c4f77e86cd9ea994f5f375b4804e50c72`. Its complete 2025 ISC notice
remains. No unattributed D590xx FreeBSD draft is imported. The existing GIC
source retains its original BSD attribution.

The pinned Raspberry Pi Linux commit
`2569533d6c33c15231fbc21704c87fcebc67173c` supplies the primary binding
[`brcm,bcm2712-msix.yaml`](https://github.com/raspberrypi/linux/blob/2569533d6c33c15231fbc21704c87fcebc67173c/Documentation/devicetree/bindings/interrupt-controller/brcm%2Cbcm2712-msix.yaml),
Git blob `c84614663b5d50eb8d5855fc30fc571f0d19ed7b`, dual GPL/BSD licensed.
Its optional `#msi-cells` is zero when present. The
[`irq-bcm2712-mip.c`](https://github.com/raspberrypi/linux/blob/2569533d6c33c15231fbc21704c87fcebc67173c/drivers/irqchip/irq-bcm2712-mip.c)
blob `49a19db2d1e1b32c5384eed7ffa9e79968553ebb` is a register/protocol
reference; Linux implementation code is not imported.

The enrolled Pi500 DTB SHA256 remains
`400b2da08abb8fd0ffd65085d1e0e1e6c6d01cadd27385403ea3a3fd5a1537f4`.
The admitted node `/axi/msi-controller@1000130000` has:

| Input | Admitted value |
| --- | --- |
| Compatible | `brcm,bcm2712-mip` |
| CPU register resource | `0x1000130000`, 192 bytes |
| Raw second reg tuple | PCI MSI message address `0xfffffff000`, 4096 bytes |
| `msi-ranges` | registered `arm,gic-400`, SPI128, rising edge, 64 vectors |
| Architectural parent INTIDs | 160..223, adding the GIC SPI offset of 32 |
| `brcm,msi-offset` | absent or zero |

Only the first resource is mapped into CPU space. Mapping the message target as
CPU MMIO would be incorrect. The DT shape, parent cells/provider, translated CPU
resource, count, flags and offset are checked before hardware writes. MIP1's
8-vector/offset8 shape is outside this selected profile and is not claimed.

The six configuration writes retain the maintained BSD contract: both VPU
masks are all ones, both host configuration words are all ones (edge mode), and
both host masks are zero. Each ordered write is read back. Runtime masking,
unmasking, EOI and affinity use the parent GIC source, as in the maintained
implementations. No invented MIP status acknowledgements or CLEAR writes are
added. These controls do not simulate interrupt delivery or analog hardware.

## Parent reservation and synchronization

The old GIC range-reservation method is void and uses assertions. Calling it
with malformed or already-used input cannot provide a checked attach failure.
Directly mapping an SPI also fails to reserve it against ordinary consumers.
The optional `GIC_RESERVE_MSI_RANGE_CHECKED` method therefore validates the entire
SPI range before one metadata commit. Implementations without that method return
`EOPNOTSUPP`. SGI/PPI, zero, overflow, non-CONFORM configuration, existing MSI
reservation or handler ownership fail without a partial claim.

All runtime GICv2 writers of reservation/configuration metadata now share its
existing spinlock: checked and legacy reservation, setup/teardown, and MSI
allocation/release. Initial source configuration precedes PIC publication.
The interrupt framework holds its source-table mutex around setup, handler-count
changes and teardown. Ordinary setup commits non-CONFORM metadata before the
framework increments handlers; teardown clears it only after the framework
has decremented them. Those transitions prevent a checked claim from stealing
an in-flight ordinary setup or teardown. No spinlock spans an allocation,
resource operation, `gic_config()` or another nested lock acquisition.

Both FDT and ACPI mapping refuse reserved MSI sources. Setup rechecks after
mapping, so an old resource mapped before a claim cannot silently become MSI.
Checked claims reject NULL/untyped setup. Legacy GICv2m NULL and typed-MSI setup
remain valid; the existing legacy reservation interface is retained.

The provider holds every parent source with individual MSI-X allocations.
A 64-vector aligned GIC MSI allocation would incorrectly require architectural
INTID160 to be 64-aligned. The local MIP bitmap handles device MSI alignment
independently, with `UINT64_C(1)` operations and an explicit full-64 mask.
This fixes the original predecessor's `1U << msi` undefined shift at slots32..63.
Child allocation, mapping and release check exact owner, contiguous group,
count/alignment and source identity. Active handlers prevent local reuse.
Callers must quiesce and tear down interrupt resources before release, as the
existing MSI framework requires; it is not a recovery API for live interrupts.

## Publication, failures and remaining gates

Complete usable state is established before MSI registry publication. A failed
registration reverts readiness. Before a parent claim, failed resource release
retains an unavailable owner for cleanup retry. After a parent claim, every
failure retains parent reservations, acquired sources, mutex and MMIO. No
parent unreserve or MSI unregister API exists, so these owners remain for the
kernel lifetime; detach returns `EBUSY`. The retained attach result preserves
Newbus softc lifetime and emits an unavailable diagnostic. It does not claim a
usable controller. Published providers also remain for the kernel lifetime.

```
python3 tools/daemonarchy/tests/bcm2712-mip-controls.py --cc cc --sanitize
python3 tools/daemonarchy/tests/gic-checked-msi-controls.py --cc cc --sanitize
```

Repeat with Clang. The first control compiles the complete production provider
and exercises every slot, full64 allocation, alignment, owner/group/source and
busy-release faults, raw target mapping, exact geometry, parent failure, dropped
configuration writes, publication and retained cleanup. It invokes a consumer
during registry publication to check readiness ordering.

The second compiles actual GIC functions and source metadata types. It exercises
checked and legacy reservation, all64 parent slots, ordinary setup/teardown,
FDT/ACPI refusal, legacy NULL/typed-MSI setup, unsupported default, and a claim
injected between actual map and setup commit. Late busy input leaves the whole
range unchanged. Fake locking detects nested holds; these deterministic controls
do not establish native concurrency. The source workflow runs both sanitized
controls. The optional FDT source entry includes the new driver; fresh native
GENERIC compilation must include the generated interface and GIC dependencies.
No aggregate kernel build runs on the operator desktop.

## Native build and compatibility checkpoint

Sealed implementation `524d75132ad0beece864daf45431068490609f8e` passes a fresh
allocated-worker ARM64 GENERIC build from empty objects, including the generated
GIC interface, GIC implementation and MIP driver. Kernel SHA256 is
`7a76572b232ea1db5d086cbc00bb3b5bd7c53d694d247b5d192e8dfd41f1094a`.
Source and downloaded object/kernel hashes match all 33 native records; the
checked reservation descriptor and linked provider methods are present. Twelve
installed tools, 30 dynamic libraries and 278 compiler resource headers were
inventoried before and after the build with unchanged identities. This binds
the installed bootstrap; canonical SDK admission remains open.

Fresh images containing this exact kernel pass all five renewed Pi4 controls:

| Control | 2 GB | 8 GB |
| --- | --- | --- |
| FDT/SD root and native GENET | 26 checks | 27 checks |
| FDT/USB-only EFI boot, USB root and native GENET | 34 checks | 35 checks |
| UEFI/ACPI USB root, firmware memory cap disabled | — | 20 checks |

Networking controls verify independent 1 MiB transfers in both directions,
round-trip hashes, zero packet errors and durable diagnostics. All seven probes
complete successfully; SD controls retain 15 files and USB networking controls
retain 18. USB-only controls verify the EFI USB load path and the mounted root's
GEOM chain to `da0`, with no SD disk attached. All five controls observe natural
guest reset and unchanged input images. Native image readback also verifies the
actual UFS kernel/loader, configuration and root ownership.

These runs establish compatibility on the new shared GIC bytes, not BCM2712 MSI
delivery or native concurrency. BCM2712 PCIe host, RP1 BAR/MSI-X nexus and clocks
remain separate missing prerequisites; MIP preparation alone does not provide
RP1 operation. The selected PCIe/RP1 DMA ranges require real address translation;
the BCM2711 identity-DMA host cannot be enabled by a compatible-string change.
