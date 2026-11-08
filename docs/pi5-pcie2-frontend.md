# Selected BCM2712 PCIe2 frontend preparation

This follow-on is confined to the enrolled Pi500 D0 DT snapshot SHA256
`400b2da08abb8fd0ffd65085d1e0e1e6c6d01cadd27385403ea3a3fd5a1537f4`,
node `/axi/pcie@1000120000`. The current checkpoint provides a pure checked
plan, window encoders and startup state engine, plus checked MIP publication.
The Newbus host wrapper is still being implemented. This checkpoint does not
register a host bridge, enumerate a device or qualify BCM2712/RP1 hardware.

## Snapshot admission and custody

The raw fixture is `tools/daemonarchy/tests/fixtures/pi500-pcie2-cells.json`.
It records actual DT cells, not narrative geometry. In particular the eight
VDM bytes are `[8,8,8,9,10,10,11,11]`, producing `0xbbaa9888`.
The raw reset handles `0x23/0x24`, MSI handle `0x27` and GIC handle `1` are
snapshot-specific OFW handles, not hardware constants. A semantically equivalent
tree with renumbered handles is currently refused before MMIO. The wrapper must
still validate each actual node/provider/resource identity.

The complete Mark Kettenis ISC notice is retained with the new engine/header.
The hardware sequence and 54MHz MDIO table are adapted from OpenBSD
`sys/dev/fdt/bcm2711_pcie.c` at commit
`05f77a90390525396f20071b9d918f94fef30e02`, blob
`c3d58d4007dab20c48ad368f5c351700a0be0e94`, SHA256
`d528fdd2ea12852f8fce738d1174c03156845b927614d0e59d5c3458c7aa2f85`.
OpenBSD's MEM32/window0-only outbound helper is insufficient for the second
64-bit tuple. Additional register field facts, multiwindow formulas, UBUS/QoS
workarounds and ordering are referenced from raspberrypi/linux commit
`2569533d6c33c15231fbc21704c87fcebc67173c`, controller blob
`59743e0a053571e015edb8166e71b72eb0b58a1e`, SHA256
`ebede451e8383497cc57911a5611ec4b5672d109890967ff031ba0e614d7b4fb`.
Linux is a hardware-fact reference; its GPL driver is not imported.

## Pure encoders and startup contract

`bcm2712_pcie_plan_make()` validates the complete bounded raw-cell shape and
scalar profile before replacing its output. The plan contains two outbound
software resources, four programmed outbound windows, three inbound roles and
ten programmed inbound windows. All seven unused inbound windows remain
zero/disabled. The separate policy contains PEER and RAM roles; raw MSI retains
its own type and unchanged bus address.

Outbound window0's exact software length is `0xfffffffc`. Its MiB-granularity
hardware decode ends four bytes later at CPU `0x1fffffffff`; those last four
bytes must remain outside the resource allocator. The second window retains
its full 64-bit PCI and CPU addresses. All encoders validate inclusive last-byte
arithmetic as `base + (length - 1)`, including valid highest PCI64 endpoints,
and leave poisoned outputs untouched on failure.

The startup engine uses injected read/write/reset/delay/dependency/publication
operations supplied by the real wrapper. It performs the BCM2712 bridge release,
checked rescal pulse, bridge reset and SerDes settling, explicit inverted PERST,
checked windows, 54MHz MDIO sequence, UBUS timeouts and QoS, Gen2/x4 ceiling,
bounded link wait and final dependency check. It preserves SCB size fields on
BCM2712; the references' BCM2711 memory-controller sizing does not apply here.
There is no internal BCM2711 MSI-controller registration or RAM adjustment to
the raw external MIP target.

MDIO checks documented address fields and bounded write-completion DONE
semantics. It does not invent undocumented PHY-data readback guarantees for
the 54MHz table. Outbound field masks come from the pinned reference.
Inbound register reads use explicitly conservative full-value equality for the
admitted writes, rather than claiming an undocumented implemented/reserved-bit
mask is factual. A revision returning different reserved bits can be refused;
that is an open hardware gate, not proof of an electrical failure.

The first dependency failure touches no hardware. Once a reset/write may have
occurred, any error retains the owner and suppresses successful publication.
There is no blind rescal retry or assumed reverse electrical rollback. A failed
publication attempt also retains its owners because a child may have partially
attached. The wrapper must disable/delete or retain the actual child safely,
guard subsequent Newbus passes and refuse unsupported lifecycle operations.
Host error/AER interrupt service is not established by link enumeration.

## Controls and boundary

```
python3 tools/daemonarchy/tests/bcm2712-pcie-controls.py --cc gcc
python3 tools/daemonarchy/tests/bcm2712-pcie-controls.py --cc clang-18
```

The controller compiles actual production translation units with address and
undefined sanitizers; Clang additionally checks unsigned overflow with recovery
disabled. Default CI uses the retained raw-cell snapshot. Optional `--dtb` checks
the actual blob/hash and enrolled properties. Controls exercise every 227
injected register/provider/reset/publication operation failure, readback faults,
bounded MDIO/link timeouts, QoS fallback, SCB preservation and output rollback.
The injected primitives are unit services, not a BCM2712 hardware simulator.
An independent oracle sealed its vectors before implementation review; its 60
API/actual-DTB comparisons pass without changing vector expectations.

The checked provider/query checkpoint `f8b858de609e2fe1d75dd2dfa2b40b8b9e100198`
passes a fresh empty-object native ARM64 GENERIC build. That checkpoint excludes
this engine and the unfinished wrapper; kernel SHA256 is
`bbe5d60a1e869a4d989613589689a02c85133f963b87075da4ed65dced305f42`.
It establishes compilation/link closure, not provider runtime or hardware MSI.
The translated RAM backend has a separate accepted native BCM2711 proxy run;
that does not emulate BCM2712 PHY/UBUS/MIP or RP1's child domain.

The wrapper must bind its base-first generic PCI fields to the actual FreeBSD
resource/method contracts, use domain2 and a full 64GiB RAM aperture, resolve
published GPIO-hog/MIP readiness before hardware access, and publish PCI children
only after checked startup. The enrolled `rp1` is a direct simple-bus container
without PCI-style `reg`; it must not be fabricated into a generic PCI OFW child
or attached directly with an incorrect inherited DMA domain. A successful root
port establishes PCI enumeration only until an explicit RP1 nexus/domain port
exists. Fresh integrated native builds, renewed Pi4 compatibility and attended
BCM2712 cold/warm measurements remain required before broader qualification.
