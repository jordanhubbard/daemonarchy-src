# BCM2712 DMA address-domain policy

This follow-on implements ordered step1 of PCIe/RP1 preparation, based on
`0ba9afc2d8ac45d15571724dc127db3e0755a0c6`. It provides pure production C
range composition and direct-map admission in
`sys/arm64/broadcom/bcm2712_dma_policy.c` and its typed header. It performs no
MMIO, allocation, busdma callback, device attachment or consumer publication.
It does not implement a busdma backend or establish native DMA acceptance.
Previously sealed exports, physical media and boards remain unchanged.

## Inputs and address domains

The fixture `tools/daemonarchy/tests/fixtures/pi500-dma-ranges.json` retains only
the raw DMA cells from enrolled Pi500 DTB SHA256
`400b2da08abb8fd0ffd65085d1e0e1e6c6d01cadd27385403ea3a3fd5a1537f4`.
The DTB's DMA aperture size is not an inventory of actual installed DRAM.

| Root PCIe2 window | PCI device address | CPU physical address | Length |
| --- | --- | --- | --- |
| Peer | `0` | `0x1f00000000` | `0x400000` |
| RAM | `0x1000000000` | `0` | `0x1000000000` |
| Raw MSI, separate type | `0xfffffff000` | `0x1000130000` | `0x1000` |

Root windows name CPU and parent-bus addresses explicitly. Child windows name
child-bus and parent-bus addresses. Composition intersects the child's parent
interval with the parent's device interval; it emits CPU-to-child ranges, not
a second adjustment in the original parent domain. The immutable snapshot
retains root/parent/child indices and full space flags.

RP1's first tuple maps child `0x1000000000` to parent PCI
`0x1000000000`; its third maps child `0` to the same parent address. Both
therefore alias CPU RAM at zero. Physical overlap with the same role is retained.
Overlapping device-domain targets or conflicting physical RAM/peer roles are
invalid. The low alias's `0x02000000` annotation must compose with the parent's
`0x43000000` RAM tuple: memory-width annotations do not truncate address cells
or replace a child's actual device mask.

RP1's peer tuple declares `0x410000` bytes, while the parent inbound peer window
covers `0x400000`. The composed peer window is clipped to 4 MiB; its 64 KiB tail
is absent. This does not shorten the separate CPU outbound MMIO/BAR mapping.
Raw MSI has a distinct representation and no data-translation operation in this
module. The policy never adds the RAM offset to an MSI message address.

## Immutable snapshots and queries

Construct the snapshot while unpublished, then pass it as `const` to queries.
There are no mutation operations for a live snapshot, reference owners, callbacks
or concurrent publication claims. Constructors use private bounded staging and
replace outputs only after complete validation. Query failure leaves every output
byte and count untouched. The caller retains ownership of the snapshot and its
lifetime; a future backend must provide actual tag/map ownership separately.

`bcm2712_dma_policy_root()` validates the complete typed root list.
`bcm2712_dma_policy_compose()` validates every child tuple before composition,
including a tuple with no intersection. Child order followed by parent order
preserves deterministic alias priority. Intersections clip to every ancestor;
holes are not filled and unrepresentable endpoint arithmetic is rejected.

`bcm2712_dma_policy_eligible()` returns CPU byte preimages clipped by the device
mask and role. It also returns the CPU start congruence, boundary and maximum
segment metadata. The byte interval alone does not claim that every possible
start or transfer length meets alignment and boundary requirements.
A 64-bit RP1 consumer can use the first high alias. A 32-bit RP1 consumer has
only the low alias's physical prefix below 4 GiB. A future bounce backend must
allocate backing in that physical preimage. This is a DMA eligibility restriction,
not a system RAM cap. A root 32-bit PCI master has no eligible low RAM alias.

`bcm2712_dma_policy_map()` checks physical coverage first, separately from
constraints. It chooses the longest eligible bounded prefix, with declared range
order breaking ties. A nonfinal prefix must end at an eligible aligned start in
some next alias. Segment lengths need not themselves be alignment multiples:
CPU `[0,3)` mapped to device `[0,3)`, then CPU `[3,7)` to device `[4,8)`, is legal
at alignment4. Rounding the first length3 down to zero would be incorrect.

Coalescing requires the same compiled alias, contiguous CPU and device addresses,
and all maximum-segment/boundary limits. It never joins a physical hole or mixes
roles. Inclusive-last-byte arithmetic admits base `UINT64_MAX`, length1, but
rejects zero lengths and wrapping last bytes. A zero device mask admits only
address zero. Root/compose need a nonempty list; an empty map request succeeds
with zero output segments. Input/output limits are explicit constants.

Errors distinguish invalid shape/constraints (`EINVAL`), uncovered or wrong-role
physical bytes (`ENXIO`), and constraint/capacity/canonical admission refusal
(`EFBIG`). The mapper is deliberately bounded and greedy. `EFBIG` does not prove
that no alternative legal cover exists. The controls retain a concrete example:
A maps CPU `[0,6)` to device `[0,6)`, B maps CPU `[4,8)` to device `[8,12)`, and
C maps CPU `[6,7)` to device `[16,17)`. At alignment4 a whole-span request for
CPU `[0,8)` greedily chooses A6 and reaches a dead end, although A4+B4 is legal.
This named conservative refusal is not hidden as a mathematical impossibility
or an optimal planner. A future backend must decide its explicit retry/bounce
behavior and must not publish partial output on such refusal.

## Controls and remaining work

```
python3 tools/daemonarchy/tests/bcm2712-dma-policy-controls.py --cc cc --sanitize
python3 tools/daemonarchy/tests/bcm2712-dma-policy-controls.py --cc clang --sanitize
python3 tools/daemonarchy/tests/bcm2712-dma-policy-independent.py --cc clang
```

The controls compile the actual production C translation unit at `-O2`, separately
from the tests. GCC runs address/undefined sanitizers; Clang also checks unsigned
integer overflow. Endpoint and congruence arithmetic avoids even intermediate
unsigned wrap, including the highest-address cases. The fixture loader only decodes enrolled address cells into typed
inputs. Controls cover aliases and masks, physical preimages, flags, peer tail,
MSI separation, nonuniform alignment transitions, explicit greedy admission,
segmentation/coalescing, malformed/wrapping/zero/extreme inputs and poisoned
output/count preservation. The maintained independent comparison has 56 controls: 40 map vectors,
6 eligibility queries, 7 constructor/rollback cases and 3 capacity failures.
A separate agent sealed these vectors from the raw enrolled DTB before reading
implementation. They remain byte-identical in
`fixtures/bcm2712-dma-policy-independent.json`, with a separate independently
captured cell inventory. Default CI reports `retained-cell-snapshot`; optional
`--dtb` verifies the exact enrolled blob and its actual properties. Both modes
pass against the same production C API. Neither host sanitizers nor an oracle
comparison establish native DMA execution.

The source workflow watches the policy and runs sanitized controls.
Those controls also compile the `_KERNEL`/`-nostdinc` path against the actual
repository ARM64 headers; Clang uses a FreeBSD ARM64 compile-only target. This
checks include closure; the native checkpoint below provides the separate
GENERIC and CPU-only unit results for sealed step1. The original step1 entry
was optional FDT. The [translated RAM busdma follow-on](pi5-translated-ram-busdma.md)
makes the pure policy a standard ARM64 dependency without registering a device.
That backend is a separately reviewed software candidate: physical bounce/sync
state, all load forms, WAITOK deferred
callbacks, device-domain constraints, child-tag inheritance and retained owner
lifetime must all qualify through their separate host and native gates. Rewriting only successful callback
addresses would bypass or duplicate translation on deferred paths. PCIe/RP1
consumer publication must wait for that coherent backend and its native gates.

## Native compile and CPU-only unit checkpoint

Sealed implementation `2ce659ffd0aed315f369e9aceab2168f63f2104e` passes a fresh
allocated-worker FreeBSD ARM64 GENERIC build from empty objects. The actual
policy object is compiled and all four exported policy functions are present in
the linked kernel. Kernel SHA256 is
`a48e8de934d187253694be9a0553f80708cbcc6efccaa563f9e4a50c1c5fb7ed`.
Source and downloaded object/kernel hashes match all 36 native records.
Installed tool, library and compiler-resource identities remained unchanged
across compilation and unit execution; canonical SDK admission remains open.

The original C controls also compile and execute on that FreeBSD ARM64 worker,
using its native headers. Five source/fixture/executable hash records are checked
independently. The separate production-API oracle compiles and loads an ARM64
FreeBSD shared object and passes all 56 controls in `actual-dtb-verified` mode
against the enrolled blob. Its portable `sys/stdint.h` bridge includes the
target's standard `stdint.h`; it supplies no fake kernel types or DMA behavior.

These are native compilation and CPU-only userland executions of sealed step1.
That kernel has not been claimed as a boot-tested image; at that checkpoint no
DMA backend or consumer called the policy. These results do not qualify the
later translated RAM backend. The earlier five Pi4 compatibility boots remain
bound to their separate MIP/GIC kernel checkpoint. Native DMA, deferred/bounce/cache behavior
and BCM2712/RP1 hardware acceptance remain open.
