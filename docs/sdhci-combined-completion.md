# SDHCI combined SDMA completion

A controller can report `SDHCI_INT_DMA_END` and `SDHCI_INT_DATA_END` in the
same interrupt. The existing handler treats this as an intermediate DMA
boundary before finishing the transfer. For an eight-byte SCR read, it copies
eight bytes, advances the offset by the 512 KiB bounce-buffer size, restarts
DMA, and then finishes the same transfer. The unsigned remaining length
underflows and the final copy writes 512 KiB outside the advertised buffer.

The candidate reloads the SDMA bounce buffer only when the interrupt reports
a DMA boundary without transfer completion. A combined interrupt goes directly
to the existing final-data or platform-completion path. Intermediate boundaries,
error handling and command-response restoration retain their existing behavior.
No DMA policy, bounce-buffer size, controller quirk or SDIO negotiation changes
are part of this source patch.

## Focused regression controls

Run the retained production-excerpt test outside the source tree:

```sh
python3 tools/daemonarchy/test_sdhci_completion.py --output /absolute/new/output
```

The test compiles both `sdhci_data_irq` and `sdhci_finish_data` verbatim, the
original `mmc_data` definition, and literal constants from the original SDHCI,
MMC and bus-DMA headers. Kernel service and remaining structure definitions
are explicit stubs; these checks do not establish ABI or hardware coverage.
`--source` selects retained baseline inputs. `--cc` binds a compiler command;
`--sanitize` enables address and undefined-behavior sanitizers.

The nineteen cases cover combined completion of tiny, sector-sized and full
buffers, final partial/full buffers after an earlier boundary, writes,
completion before the command response, ordinary completion, intermediate
read/write boundaries, aggregation masking, platform completion, CRC error
resets, busy-only commands and absent commands/data. A larger allocated arena
surrounds the advertised buffer so baseline overrun detection uses independent
guard-byte assertions without causing a test-process out-of-bounds access.

Native FreeBSD ARM64 and AMD64 controls pass all nineteen cases with the
candidate and report nine passes and ten assertion failures with the original
implementation. Linux Clang 18 ASan/UBSan produces the same results. The initial
native harness compilation refusal due to a system macro redefinition is
retained; the final harness preserves the native definition when available.

## Runtime investigation and limits

The existing native MMCCAM kernel with the separate CMD5 fixes reproduces this
panic during an eight-byte CMD51 read under an observed private patched QEMU
10.2.4 controller model. The trace reports response, DMA-end and data-end
together (`0x0b`); the panic stack includes `sdhci_finish_data` and the remaining
length register contains `8 - 512 KiB`. The VM exits with status zero because
`-no-reboot` terminates it after the guest panic. This is a failed boot, and its
unchanged virtual-card bytes and terminal state were independently checked.

Installed QEMU 8.2.2 has a separate SDMA restart defect: the same kernel reads
through 512 KiB correctly, but a 1 MiB read times out after the first boundary.
The trace records the guest writing the restart address while the model keeps
the advanced address and half the block count. The private model contains an
active-SDMA restart repair. All four unchanged original controller tests from
its owning project pass there; the older binary lacks the required qtest
accelerator, so its oracle failure is an infrastructure limitation, not a
negative assertion control. This comparison does not isolate every private
model modification or qualify that binary as a canonical SDK resource.

A fresh native ARM64 `GENERIC-MMCCAM` kernel and the selected `sdhci` and
`sdhci_pci` modules built successfully. The kernel SHA-256 is
`929bf42482b25927d39c14f1d56a2cfeccccf6f824ae390f01f6b76519afcdad`.
The build composes the original F4 source with two frozen CMD5 overlays from
SDIO review head `5c74cafb7e8b77d74dc7d083b33cad361d3aafc0` and this SDHCI
overlay. It does not establish all-module or complete review-head qualification.
Both 115,681-entry source trees, the 1,113-node original bootstrap and the worker
kernel/tools retain their recorded identities. The 110 GiB free-space floor was
preserved, including private image staging and persisted readback.

A fresh generic ARM64 UEFI VM on the observed private controller model boots
past the previous CMD51 panic. The running build identity and kernel-file bytes
match the new native artifact. Global and slot quirks are zero. All six reads
from 512 bytes through 1 MiB return the exact expected data. Full 64 MiB guest
reads before and after an exact 1 MiB write at offset 4 MiB match the seed and
marker expectations, with every other byte unchanged. Actual PID exit was
observed through a pidfd, the unit is inactive with `MainPID=0`, and an
independent whole-card backing-file comparison after shutdown matches the guest
readback. The first sentence of the retained runner's scope inherited its
read-only size-sweep description; a retained clarification records the actual
bounded write, which is also explicit in its commands and result fields.
The same new kernel on installed QEMU 8.2.2 still reads through 512 KiB
correctly and fails the 1 MiB read with EIO after about ten seconds. That fresh
control shuts down cleanly with actual PID exit observed and the entire virtual
card unchanged. The kernel repair does not fix the old model's restart defect.
The previous kernel and original failed fixtures remain retained.

Physical Pi SD/SDIO, all-platform native SD and Ethernet, Wi-Fi traffic,
performance, the separate MMCCAM discovery warning, canonical SDK/lifecycle,
per-unit packages, install/update/rollback and full release acceptance remain
open. This candidate is independent of the operator's existing Pi 5 image.

The [curated development record](../verification/sdhci-combined-completion-20261122.json)
binds the selected inputs and scoped results. Raw logs and replay media remain
private; no passing canonical receipt is claimed.
