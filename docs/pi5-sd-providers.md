# BCM2712 D0 SD provider preparation

This follow-on supplies the pin-control and voltage-settling portions of the
initial on-SoC SD path missing from source `73260396`. It does not establish
BCM2712 hardware operation. The AON GPIO provider and checked pinctrl consumer
are separate changes that must be integrated before a candidate can resolve
all its dependencies. No physical board or physical media was changed.

The enrolled Pi500 DTB has SHA256
`400b2da08abb8fd0ffd65085d1e0e1e6c6d01cadd27385403ea3a3fd5a1537f4`.
Its `mmc0` alias selects `/soc@107c000000/mmc@fff000`: host CPU address
`0x1000fff000`, CFG `0x1000fff400`. `sw_sdio` resolves to the existing
`fixed-clock` provider at 200 MHz; it does not require a firmware mailbox clock.
The GIC400 SPI273/INTID305 provider also exists. RP1 PCIe, Ethernet, USB and MSI
are not dependencies of this initial SD host.

## Pin controller

`bcm2712_pinctrl.c` matches only `brcm,bcm2712d0-pinctrl` and
`brcm,bcm2712d0-aon-pinctrl`. The D0 main pin table is adapted from OpenBSD
`sys/dev/fdt/bcmstbpinctrl.c`, CVS revision 1.3, 2025-09-19, Git blob
`76647bfb5ae305be07b339f649c7f57a7aa8636a`. Mark Kettenis's 2025 ISC copyright,
permission and disclaimer remain in the driver and table header. C0 layouts and
other undeclared functions are not advertised.

The selected SD group requests pull-ups on `emmc_cmd` and `emmc_dat0..3`.
At CPU `0x107d504100`, these are fields in offset `0x18`: mask `0x000ff0c0`,
pull-up value `0x000aa080`. These dedicated pins have no mux. The AON group at
CPU `0x107d510700` selects `aon_gpio5` function `sd_card_g`: offset `0x0c`,
shift20, selector4; its pull-up is offset `0x14`, shift28, selector2.
Other fields survive every read-modify-write.

GPIO2 (RP1 RUN hog), GPIO3 (SD I/O voltage select) and GPIO4 (SD power) have
explicit GPIO mux support. Their D0 mux register index3/field2..4 and pad
index5/field11..13 were checked against Raspberry Pi Linux commit
`2569533d6c33c15231fbc21704c87fcebc67173c`, file
`drivers/pinctrl/bcm/pinctrl-bcm2712.c`, Git blob
`c349bcf05391aee45833ffdd76879437b541fa8e`. Linux is a register-contract
reference; no Linux implementation is imported.

Because this DTB has no `gpio-ranges`, the AON GPIO driver resolves the registered
sibling compatible and uses standard `FDT_PINCTRL_SET_FLAGS` before changing
GPIO direction. The provider verifies the sibling's compatible, parent, register
shape and bank widths. Physical pins2/3/4 are set to GPIO mux0. CD5 input keeps
and verifies native SD-card mux4; output requests fail. OUTPUT preset flags are
allowed so the GPIO provider can preload DATA before enabling output. Other
unsupported flags fail. The pin controller does not change GPIO DATA or IODIR.

The selected tree also enables SPI default groups on main GPIO1..4 and UART
(default Bluetooth connection) groups on GPIO24..27/29. All their mux and pad
fields are supported: GPIO2 uses vc_spi0 selector7, GPIO3/4 selector6, and
GPIO24..27 uart0 selector4. Main GPIO1/29 use GPIO selector0. These D0 register
facts come from the same pinned Linux tables. They are needed because attach
validates every owned enabled default group, even when its consumer driver is
outside the initial SD bootstrap. This does not qualify SPI or UART operation.

The driver attaches at `BUS_PASS_BUS + BUS_PASS_ORDER_EARLY`. It validates all
its enabled default groups before performing any MMIO or publishing provider
xrefs. This local walk propagates failures that the existing generic
`fdt_pinctrl_configure_tree` discards. Each later group is also planned in full
before writes. Unknown pins/functions/properties, conflicting biases and
malformed strings fail. Register updates are serialized. Detachment returns
EBUSY because the existing framework has no provider-unregister operation.
The GPIO provider must attach later, followed by GPIO-regulator consumers.

## Regulator delay

The Pi500 VQMMC GPIO regulator declares `regulator-settling-time-us = <5000>`.
The existing GPIO regulator driver ignored it. The voltage callback now returns
the greater of this delay and its existing startup-delay floor, after all GPIO
writes succeed; the regulator core consumes that delay before the next consumer
operation. Enable timing retains its existing startup-delay semantics. Missing
settling time defaults to zero. Malformed cells and unsigned values exceeding
INT_MAX fail parsing instead of becoming a negative delay. Both enable and
voltage callbacks initialize the returned delay to zero before any failure;
the checked voltage framework consumes the returned value even on an error. This change does
not implement directional settling-time properties or analog regulator physics.

## Controls and remaining acceptance

Run:

```
python3 tools/daemonarchy/tests/pi5-sd-provider-controls.py --cc cc
```

The harness compiles the production pin parser/default-plan function bodies,
shared pin plan/apply code, and production regulator callback/delay parser
against bounded OF/GPIO/MMIO substitutes. It checks exact SD pad and mux fields,
untouched register bits, all-or-nothing group validation before MMIO, unknown
and malformed configurations, failed GPIO writes, unsupported voltages,
5000us delay returns, startup-delay compatibility, zero initialized delays on
GPIO/no-state failures, and malformed/overflow cells. The fixture
`tools/daemonarchy/tests/fixtures/pi500-pinctrl-defaults.json` retains exact raw
properties fetched from the enrolled DTB: all enabled D0-owned default groups
(including nested UART/SDIO groups), disabled AON I2C and a foreign RP1 group.
The production default walk builds 37 main and two AON register operations;
independent expected register masks check the complete main update. Substituting
an unsupported SPI function rejects the plan before register access.
It verifies software behavior in those controls. It does not exercise Newbus
attachment, real clock/reset/pad hardware, GPIO electrical levels, actual elapsed
voltage settling or a BCM2712 card transaction.

A fresh combined native build must include these changes, the AON GPIO provider
and the opt-in checked pinctrl consumer. Then repeat the existing Pi4 FDT and
ACPI/USB compatibility controls against the exact new kernel. Those controls
cannot qualify BCM2712 SD providers. Physical Pi500 boot, SD command/data traffic,
voltage/power sequencing, card detect and removal remain open acceptance gates.

## Failed regulator initialization ownership

The GPIO regulator now opts into `REGULATOR_FLAGS_DEFER_SYSCTL`. Existing
regulator consumers retain their immediate diagnostic publication behavior;
this provider publishes diagnostic callbacks only after `REGNODE_INIT` succeeds
and the regulator list/FDT xref is published. Publication runs outside the
regulator topology lock.

The new `regnode_destroy_unpublished()` API is limited to an exclusively
producer-owned, unregistered node. It rejects published nodes, enabled nodes,
consumer references and consumer handles. Existing diagnostic contexts must
drain before any backing allocation is freed; a busy context leaves the node
and its producer ownership unchanged for retry. The drain holds neither the
node nor topology lock, since an active voltage handler can acquire both.
Successful destruction releases copied strings, producer softc, sx, kobj class
reference and node using their original allocation owners. Static strings are
preserved.

Failed GPIO regulator attachment destroys its private node before releasing
state/enable GPIO handles, state/pin arrays and OF-owned parsed names. It returns
an error rather than reporting an attached but unpublished regulator. The
private deferred node has no diagnostic callbacks to retain a failed Newbus
producer. This is allocation/handle cleanup; it does not claim generic restoration
of pin mux or physical power state after a partly applied hardware operation.

```
python3 tools/daemonarchy/tests/run-regnode-lifetime.py --cc clang
```

This control compiles the actual allocation-bearing production types and
create/register/destroy, GPIO parse/init/attach/unwind functions against bounded
allocation, SYSCTL, OF and GPIO services. It checks deferred publication,
registration/initialization failures, partial pin acquisition, successful retry,
correct malloc/OF owners, static strings, and busy diagnostic-context retry.
Clang and GCC ASan/UBSan controls pass. This does not establish live SYSCTL
concurrency, native kernel linking, hardware rollback or physical rail behavior;
the newly integrated source still requires a clean native build.
