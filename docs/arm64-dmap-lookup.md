# ARM64 direct-map lookup during mapping updates

An eight-GiB synthetic PCIe2 guest exposed recursive kernel faults before init.
The retained kernel was built from `38edc1cb9562556ff32fd02904409a9f3713bada`;
its full ELF SHA256 is
`fcb7d7f5eaacf41dc5ba5bee30ddbe8e5a084ecf3510887f4998632d701429e6`.
CPU0's oldest saved fault was a read in `bcm_sdhci_dma_intr()` at
`0xffff000000a35a4c`, accessing `0xffffa00104266248`. CPU3's oldest saved
fault was in `arm_gic_intr()`, accessing `0xffffa0010412d888`.

Both handlers entered `pmap_klookup()`, whose software walk then faulted while
reading its own direct-mapped table at `0xffffa001044ac110`. Repeated saved
translation faults (`ESR 0x96000006`, ELR `0xffff00000094e6a8`) exhausted the
exception stacks. Bounded emulator interrupt logging confirmed terminal writes
below the stack guards at the first vector-save instruction, with
`ESR 0x96000047`. No init, USB payload test or natural reboot was accepted for
this failed run.

All three physical addresses lie in the same 32-MiB direct-map group,
`[0x104000000,0x106000000)`. The saved table contains structurally consistent
entries; that group's contiguous bits are clear and it contains a demoted
mapping. The debug walker resolves the addresses after the stall. These facts
are consistent with a concurrent break-before-make update, but do not establish
the initial writer's chronology or prove a stale emulator TLB or DMA overwrite.

## Lookup contract

`pmap_klookup()` already retries mappings whose valid bit is temporarily clear.
Its software fallback must not depend on a direct mapping that the same update
can temporarily invalidate. The change records the exact physical start and
exclusive end of each interval constructed by `pmap_bootstrap_dmap()`. After
the final bootstrap invalidation and publication, a failed hardware translation
of an address within the direct-map bounds uses those intervals to determine
membership and compute the physical address without walking page tables.

The inventory is separate from `physmap` and allocator availability. Those
lists are rebuilt after kernel, table, firmware and other allocation exclusions;
excluded pages can remain mapped. Bounds alone are also insufficient: physical
page zero, a leading gap and holes between RAM regions need not be mapped.
The snapshot retains the actual page-rounded start and actual constructed end.

Hardware translation success, lookup before bootstrap publication and lookup
outside the constructed direct-map bounds retain the existing paths. A failed
membership check leaves the caller's output unchanged. The fallback remains in
the existing `NO_PERTHREAD_SSP` function and performs no allocation, locking or
thread-dependent access.

Supported ARM64 callers preserve direct mappings while changing their attributes
or granularity. The generic low-level removal APIs do not all enforce this
contract with assertions; this inventory does not add support for arbitrary
direct-map removal or runtime memory hotplug.

## Qualification boundary

Host controls must compile the actual lookup and bootstrap code, preserve holes,
NULL outputs, byte offsets, nonzero physical bases, early lookup and ordinary
page-table fallback, and refuse access to unreadable table storage during the
direct-map retry. These controls do not establish guest progress.

```
python3 tools/daemonarchy/tests/arm64-dmap-lookup.py --cc clang
```

The controls extract the complete production functions. Typed host services
stand in for bootstrap construction and MMU operations; a protected host page
detects an attempted fallback-table access. An optional `--baseline-source`
can bind the original function as the negative control. The local GCC13,
Clang18 and Clang23 runs pass 86 checks with address/undefined sanitizers.

A fresh native kernel, actual selected PCIe2 MSI-X and MSI USB transfers,
independent durable kernel and payload hashes, exact interrupt/stray counters,
natural reset and ordinary Pi4 simulator regressions remain required. Physical
Pi4/Pi500 and RP1 qualification remain separate. No passing native or physical
acceptance is claimed by this preparation.
