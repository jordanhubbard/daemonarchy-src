# BCM2712 translated RAM busdma

This branch adds the second PCIe/RP1 prerequisite after the
[immutable address-domain policy](pi5-dma-policy.md). It is based on
`2ce659ffd0aed315f369e9aceab2168f63f2104e`. The new opt-in constructor and
ARM64 bounce integration are software candidates. No PCIe/RP1 frontend calls
them yet, no consumer is published, and physical boards remain untouched.
Host controls do not establish native busdma execution or device acceptance.

## Admission and address domains

`bus_dma_tag_create_bcm2712_ram()` accepts a physical bounce parent or an
already translated parent plus an explicitly composed child policy. The public
signature is in `sys/arm64/include/bus_dma.h`. The policy is copied before
publication; ordinary `bus_dma_tag_create()` descendants inherit its immutable
context and remain in the same device domain. An explicit constructor with a
new composed policy changes domain while retaining the parent's physical
preimage, alignment, boundary, coherence, domain preference and lifetime.
It does not reinterpret a parent device address as a CPU physical address.

Admission covers the enrolled root/RP1 affine RAM geometry: CPU base zero,
64 GiB aperture, one or two declared aliases at device base zero or 64 GiB.
The aperture describes reachability, not installed RAM. Declared alias order is
preserved. A high alias is selected when its device mask admits it; RP1's low
alias permits a 32-bit child and physical bounce backing below 4 GiB. A root
32-bit master without that alias is refused. `common.lowaddr` remains a device
constraint; a separate checked physical prefix drives bounce allocation,
ordinary DMA allocation and NUMA matching.

This is deliberately narrower than the general policy. `highaddr` must be
`BUS_SPACE_MAXADDR`; `lowaddr` must be a page-ended contiguous low-bit mask.
Both parent and child alignments must be powers of two no larger than a page;
nonzero boundaries must be powers of two no larger than 64 GiB. These offsets
preserve the admitted alignment and boundary congruences. Tags require bounded
positive transfer sizes, up to `(INT_MAX - 3) * PAGE_SIZE` and the checked size
roundup limit, and 1 through 65535 segments. Unrestricted container tags must
choose explicit sufficient bounds. IOMMU parents, Realm operation, arbitrary
translated windows, unsupported exclusion windows or flags and unsupported
geometry fail before a translated tag is published.

Peer/MMIO DMA is excluded. Source pages must be real physical RAM from
`vm_phys_paddr_to_vm_page()`, not the broader fictitious-page fallback of
`PHYS_TO_VM_PAGE()`. A source overlapping an explicitly declared peer window
is refused even if presented as a page. Managed RAM above a device's direct
preimage can bounce into eligible backing. Raw MSI addresses use their separate
policy type and are never passed through RAM translation.

## Loads, bounce copies and lifetime

Only segment output crosses into device space, before the existing segment
builder. Bounce addresses, physical page lookups, cache maintenance lists and
DMA allocation/free addresses stay physical. The existing builder enforces
segment lengths, segment counts and device boundaries; the opt-in hook checks
alignment at each new segment start. It does not rewrite completed callbacks.
All MI forms reach the actual physical/buffer builders, including page arrays,
physical/virtual lists, UIO and ordinary/external-page mbufs.

WAITOK preparation walks the original MI memdesc with a private count-only
state and an internal callback. It performs no bounce allocation, segment
publication, cache operation, data copy or driver callback. It records errors
for the real load despite `map_waitok`'s void signature. The first real chunk
reserves the whole transfer before mapping any chunk, so a multi-buffer replay
cannot inherit partially consumed state. NOWAIT failures unwind partial chunks,
active pages and remaining reservations. The error callback runs exactly once.

Queued/replaying maps retain their tag and parent chain. Pending maps reject
new loads and destruction; sync/unload cannot release a queued owner's pages.
At completion, the pending marker clears while the driver lock is held. The
worker has already saved its unlock function/argument and never dereferences
the map/tag after the driver callback. A callback can unload and destroy its
map/tag, or unload and start another load. A waiter acquiring the lock after
unlock sees no artificial post-unlock busy window. The caller-owned driver
lock argument must remain valid through unlock, as in the existing API.

Tags hold checked references to their immutable context and parent. Reference
overflow refuses admission. Destruction reports busy while maps or translated
children remain. Domain changes refuse busy owners and restore the committed
domain/zone/flags on failure. Allocation kind is recorded per translated map,
so one allocation cannot change how another is freed. Failed map allocation
does not leave a phantom zone map count. Bounce zones and their pooled pages
retain the existing global kernel-pool lifetime; failure does not publish a tag
or leave a dangling tag/map reference in those pools.

Callers retain the ordinary busdma obligation to serialize tag/map operations
and keep source mappings/pages stable. These controls do not establish native
concurrent execution or physical cache-coherency acceptance.

## Templates and compatibility boundaries

`bus_dma_template_clone()` copies values; it cannot borrow a source-tag pointer
that may be destroyed before `template_tag()`. ARM64 therefore stores a
by-value translated-template refusal marker in existing padding at offset52.
Static assertions retain the 96-byte template ABI, `maxsegsize` offset56 and
`name` offset88. Init and every clone initialize the marker; fill cannot clear
it. A translated clone returns `EOPNOTSUPP` and NULL output before reading any
possibly stale parent. It introduces no borrowed reference or hidden leak.
Owned-by-value translated templates remain a separate unsupported feature.
Selected USB/xHCI and cgem sources use ordinary child creation and do not use
template cloning.

Other architectures receive no template field or behavior change. Default
nontranslated mapping retains the existing path. The optional shared bounce
hooks are enabled only by ARM64; the saved-unlock logic also makes callback
map/tag destruction safe without a later dereference. Unsupported memdesc
refusal is opt-in to translated tags. The pure policy and affine admission C
are standard ARM64 source entries so non-FDT builds have a complete link
closure; neither entry registers a device.

## Controls and native gates

```
python3 tools/daemonarchy/tests/bcm2712-busdma-controls.py --cc gcc --out /path/to/new/gcc-proof
python3 tools/daemonarchy/tests/bcm2712-busdma-controls.py --cc clang --out /path/to/new/clang-proof
```

The host controller compiles verbatim production functions, actual shared
bounce operations and MI traversals, with injected VM, pool, cache and scheduler
primitives. It uses the actual template definition and emits source hashes.
GCC and Clang address/undefined sanitizers cover root/RP1 alias inheritance,
mask/geometry refusal, physical sync metadata, actual PREWRITE/POSTREAD bounce
copies, all memdesc forms, EPG header/trailer offsets, whole-transfer partial
reservation/replay, exactly-once callbacks, callback destruction/reload,
allocation/free ownership, domain rollback, partial pool allocation failures,
reference overflow, fictitious pages and poisoned template source lifetime.
These are production-function controls, not a native scheduler or hardware run.

Before adoption, the remaining gates are a fresh empty-object native GENERIC
build including the backend and its dependency closure, actual native busdma
execution with a nonzero offset, real device reads/writes through ordinary
child tags, forced high-physical-page bounce copies and deferred/error owner
controls, plus unchanged Pi4 FDT/GENET and ACPI/USB controls. A test frontend
must pair the production translated tag with an actual corresponding inbound
hardware/model window before any device children are published. Changing DT
cells alone does not invoke this API. BCM2712/RP1 electrical MSI, controller,
cache-coherency, reset and native hardware acceptance remain open.
