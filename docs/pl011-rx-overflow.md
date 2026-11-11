# PL011 receive overflow and interrupt progress

The UART receive method must move available input into the software ring and
quiesce its receive interrupt. The generic interrupt filter repeatedly calls the
device while an interrupt remains pending. In normal operation that loop has no
probe-time iteration limit; scheduling a software interrupt does not itself let
the software receiver run before the hardware filter returns.

The PL011 receive method stopped when its software ring became full. It left
the hardware FIFO unread, so the receive level could remain asserted. A finite
input burst could consequently keep the hardware filter running while the
software interrupt needed to free ring space was pending.

## Evidence and correction

Native ARM64 kernel `615c217d780f0aca890b82e844a63174a135201b520cf6882593a00acd682c4d`
reached init, completed diagnostic capture and, in one experiment, durably copied
its kernel and completed two USB payload transfers. Long console frames then
stalled without an echo. The read-only failure observations show a full PL011
FIFO, enabled/pending/active GIC INTID153 and CPU execution in PL011 interrupt
polling or the receive method; the other three CPUs were idle.

Controls over the original receive and generic interrupt functions reproduce
the scheduling condition: a 450-byte finite burst fills the 383 usable slots in
the 384-entry ring, leaving input pending while the filter continues polling.
Allowing software draining permits progress. These injected scheduling controls
are distinct from native ring and software-interrupt state observations.

The correction follows the existing NS8250 and Z8530 overflow contract. After
the normal receive loop, it discards input remaining in the hardware FIFO,
refreshing the FIFO status with the existing bus barrier under the existing
hardware lock. The receive level can then clear and the filter can return.
Queued ring bytes, ring indices and ordinary framing/parity handling retain
their existing behavior. No interrupt-mask cache or rearm callback changes.

Overflow remains lossy. The existing overrun sentinel is written into the ring's
reserved empty slot without advancing its producer, so this change does not
promise that the TTY will receive an overrun token. It does not add RTS/CTS flow
control or guarantee delivery of a burst larger than available receive storage.
Normal no-flow-control console protocols must bound or pace their frames; the
bootstrap experiment uses separate short operations without reducing its USB
payload, kernel-identity, interrupt, durability or natural-reset requirements.

## Qualification boundary

```
python3 tools/daemonarchy/tests/pl011-rx-controls.py --cc clang
```

Finite empty, fitting, exactly-full, overflowing, already-full and wrapped-ring
inputs need controls that preserve queued order, observe FIFO reads and interrupt
quiescence, and demonstrate subsequent software draining and input recovery.
The unchanged original function remains a bounded negative control. Local GCC13,
Clang18 and Clang23 runs pass 2,441 host checks with address/undefined sanitizers.

Draining an arbitrary infinite input stream is not a bounded-latency guarantee.
The measured RX-level condition does not establish receive-timeout/error
acknowledgment semantics, physical UART timing, electrical flow control or Pi/RP1
hardware acceptance. A fresh native kernel and complete bootstrap I/O/reset
experiments remain required.
