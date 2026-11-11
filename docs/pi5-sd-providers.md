# BCM2712 D0 SD provider preparation

This follow-on supplies the pin-control and voltage-settling portions of the
initial on-SoC SD path missing from source `73260396`. It does not establish
BCM2712 hardware operation. This combined source includes the AON GPIO provider,
checked pinctrl consumer and failed-regulator lifetime cleanup described below.
Their presence does not prove native attachment or that the selected DTB's
providers resolve at runtime. No physical board or physical media was changed.

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
the checked voltage framework consumes the returned value even on an error.
This change does not implement directional settling-time properties or analog
regulator physics.

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

The combined source passes the clean native build and Pi4 compatibility
controls recorded below. Those controls cannot qualify BCM2712 SD providers. Physical Pi500 boot, SD command/data traffic,
voltage/power sequencing, card detect and removal remain open acceptance gates.

## AON GPIO bootstrap consumer

The companion `bcmstb_gpio.c` adaptation retains Mark Kettenis's original ISC
notice. It accepts the selected D0 AON CPU resource `0x107d517c00`, 64 bytes,
and bank widths 15/6. GPIO numbers retain the 32-pin bank stride; unsupported
and reserved pins advertise no capabilities. Pins 2/3/4 provide polled input
and output; card-detect pin 5 is input-only. The common GPIO layer handles
active-low flags, so the controller returns raw levels.

The driver resolves the sibling D0 AON pinctrl provider before publication.
Output presets are written and ordered with a bus barrier before pad muxing
and direction changes. It also orders ordinary output writes before returning
to regulator timing code. Unsupported pull/interrupt requests fail. This
profile does not implement GPIO interrupts or the generic GPIO-hog mechanism;
capability on pin 2 alone does not establish RP1 reset release.

The provider is retained for the kernel lifetime (`detach` returns `EBUSY`),
because external rail/CD pin handles are not child-device lifetime references.
Failed attachment unregisters its bus, restores its saved DATA/direction/mask
words, and releases the resource/mutex. The actual production driver is built
against bounded OF/MMIO/kernel-service stubs for fault controls. They prove
register masks, barriers, preset ordering, active-low ownership, malformed
input refusal and cleanup behavior in the test model. They do not prove native
Newbus failure recovery or electrical timing. The native integration build is
recorded below; physical GPIO/SD evidence remains required.

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
concurrency, hardware rollback or physical rail behavior. Native linking is
recorded separately below.

## Integrated native build and Pi4 compatibility controls

A sealed export at `91204d3c8f22504eb4a346e5304d682618aea0d6` was built in
an empty object directory on native FreeBSD 16 ARM64 with Clang 21.1.8. The
first build failed because the generated pinctrl interface needed OFW handle
definitions earlier; its evidence is retained. The corrected, fresh second
export passes all changed-driver compilation, GENERIC link and strip.
Independently read-back stripped kernel SHA256 is
`8ec36736678f4de5308164fe6dfe990119b6ff042bf6eec6887ddd3ee84c60c9`;
unstripped kernel SHA256 is
`4581f18d274a92ff7456535bd88662cda8416e4a1330ce3ea1f26a9df235cab4`.
The nine changed object files and their compiler input paths were checked
independently. Later documentation and optional dependency gates leave the
implementation bytes unchanged. Native config verifies the final GENERIC
compiled closure equals that build and verifies removal of each required option.

Fresh regular-file images retain that exact kernel and the established EFI
loader; native UFS readback checks kernel/loader hashes, root ownership and
loader configuration. The 2 GiB Pi4 FDT control passes all 15 checks; its UEFI
PCIe/USB control passes all 19 checks. Both activate four CPUs, save all seven
probes and ten durable files, synchronize and reset, and preserve input images.
The USB dmesg reports the actual `_DMA` limit `0xbfffffff`. These are BCM2711
compatibility controls; they do not execute the BCM2712 providers.

The [selected AON hog follow-on](pi5-aon-gpio-hogs.md) adds GPIO2 RUN ownership
and verifies subsequent pinctrl callback writes by readback. It includes staged
common-bus lifetime repairs and production plain/OFW/ACPI failure controls.
These later implementation changes require a new native kernel build; the
preceding `8ec367…` build and Pi4 controls remain evidence for their original
sealed source, not the hog implementation.
