# Selected BCM2712 D0 AON GPIO hog ownership

This follow-on starts at reset-provider checkpoint `0d7c421fd08bcbc7c2cd2aa389b32a525ef296d1`.
It implements the selected Pi500 RP1 RUN hog through the existing AON GPIO
provider. It changes neither the qualified SD checkpoint nor its sealed images.
Host production-function controls pass; native compilation and BCM2712 pad/RP1
acceptance remain open. No physical board or media was changed.

## Selected binding and ownership

The enrolled DTB SHA256 is
`400b2da08abb8fd0ffd65085d1e0e1e6c6d01cadd27385403ea3a3fd5a1537f4`.
Its `/soc@107c000000/gpio@7d517c00/rp1_run_hog` has exactly:

```
gpio-hog;
gpios = <2 0>;
output-high;
line-name = "RP1 RUN pin";
```

The captured properties are retained in
`tools/daemonarchy/tests/fixtures/pi500-aon-hogs.json` and compiled into the
production-driver control. This remains the D0 AON resource profile at CPU
`0x107d517c00`, 64 bytes, bank widths 15/6. No generic all-board hog behavior is
introduced. Existing OpenBSD ISC notices in the GPIO/pinctrl drivers remain.
The generic GPIO bus and ACPI wrapper retain their original BSD notices.

Before mapping or touching registers, the provider validates every enabled
child. Enabled non-hog children, nested children, unknown properties, malformed
boolean/string/two-cell arrays, unsupported flags/directions/pins and duplicate
pins fail the complete plan. Disabled children are ignored. Logical output
polarity is translated once into the physical preset; GPIO5 remains input-only.
A hog may contain multiple parent-local GPIO tuples.

A new opt-in `gpiobus_add_bus_deferred()` creates an unpublished metadata/gpioc
bus. Its reserved factory flag `0x80000000` has no existing assigned gpiobus
hint-bit collision in this tree. The deferred path omits hinted, identified and
FDT consumers during initial attach and later passes. The legacy factory keeps
its existing publication behavior. All hogs are reserved with actual GPIO pin
handles before any DATA, mux or direction write. Those handles stay in the
producer softc, with no temporary global enable window.

Private application orders DATA preset and barrier, checked pinctrl mux, then
IODIR and barrier. The pinctrl callback now verifies each requested mux/pad
field after writing; GPIO verifies DATA and direction. Only a complete success
marks the provider ready and publishes its OF xref. Public set/configure/toggle
methods reject owned hog bits, including direct gpioc requests that bypass
framework reservations. Other consumers cannot acquire those held pins.
This requests RUN deassertion; it does not claim a reset pulse, reset timing,
PCIe training or RP1 operation.

## Failed attachment and bus lifetime

Before any possible hog/pad change, cleanup releases acquired handles, deletes
the bus child, restores only the interrupt masks and releases MMIO. Every
fallible deletion, mask readback and resource release is checked. Common bus
cleanup finishes child detach/deletion and interrupt rman cleanup before
freeing pin metadata or destroying its mutex. The actual child-deleted callback
owns ivars; the old second free after deletion was a use-after-free.

No complete pad rollback interface exists. Once a hog may have changed a pad,
a later error retains the unpublished parent, all handles, child/private
metadata, mutex, MMIO and pinctrl reference. Cleanup failure before that point
also retains all unreleased owners. Newbus receives success only to preserve
the unavailable owner; diagnostics and readiness explicitly report failure.
Raw reservation, later-pass/rescan/driver-added and power callbacks cannot revive
it. Published providers and partially applied hogs return `EBUSY` on detach.
A pre-write retained owner can retry cleanup. No fabricated rollback is claimed.

Plain, OFW and ACPI wrappers follow the retained-owner contract. ACPI does not
install a handler or enumerate a quarantined common bus. Handler removal errors
retain the handler and common owner; its callback rejects unavailable access.
Successful removal is tracked so a later busy-child retry cannot remove it twice.
Pin-name access also rejects unavailable state and pin indices equal to the count.

## Controls and remaining gates

```
python3 tools/daemonarchy/tests/bcmstb-gpio-controls.py --cc cc --sanitize
python3 tools/daemonarchy/tests/gpiobus-lifetime-controls.py --cc cc --sanitize
python3 tools/daemonarchy/tests/pi5-sd-provider-controls.py --cc cc
```

Repeat with `--cc clang`. The first harness compiles the complete production
GPIO driver, including parser, attach and cleanup. Faults cover preflight,
first/second reservation conflict, polarity, private ordering, raw hog writes,
first/second mux failure, dropped DATA/direction writes, child deletion and
resource release, plus retained-owner guards and cleanup retries. Pin-control
controls execute its production callback and reject dropped mux/pad writes.

The lifetime harness compiles production allocation-bearing types and function
bodies. Its fake Newbus deletion calls the actual child-deleted callback on
actual heap allocations. It covers plain/OFW/ACPI attachment, initialization
failures, busy children/rman, real ivars ownership, deferred enumeration,
legacy publication, NULL/uninitialized/quarantined reservations, name bounds,
ACPICA handler failures and retry. Address/undefined sanitizers reproduced the
old ivars UAF before repair. Bounded fakes do not establish native Newbus
concurrency, native compilation or electrical behavior. CI runs these controls;
a fresh allocated-worker kernel build and later native hardware test remain
required before any BCM2712 acceptance. PCIe/MSI/RP1 nexus/clocks remain separate
missing prerequisites described in the reset-provider document.
