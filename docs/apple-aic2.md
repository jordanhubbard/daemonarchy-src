# Apple AICv2 preparation

The first native Apple target is the M2 Pro Mac mini, Mac14,12 / J474s / T6020.
Its published bootstrap DTB uses `apple,t6020-aic`, `apple,aic2`, four interrupt
cells, and separate core and event windows. Matching only `apple,aic` cannot
attach its root interrupt controller.

The driver distinguishes AICv1 and AICv2. It obtains register-bank capacities
from the appropriate information registers, checks those capacities against
the mapped core window, and uses the separate event resource for AICv2. Older
AICv2 single-window bindings remain supported when that window covers the
event register. Hardware IRQ source slots include their die; timer FIQs have
separate source slots and accept only the four defined timer numbers.

AICv2 has no AICv1 per-IRQ target registers. External IRQ delivery stays on the
boot CPU, with secondary CPUs retaining their FIQ timers and fast IPIs. An
explicit affinity request excluding the boot CPU returns `EOPNOTSUPP` rather
than writing an AICv1 register on AICv2. This does not reduce the CPU inventory
or qualify secondary-CPU startup.

The register layout and IRQ routing use the ISC-licensed OpenBSD
[`aplintc.c` reference](https://github.com/openbsd/src/blob/0a7b23cd6ea2cf5e16250214e2200683622390e7/sys/arch/arm64/dev/aplintc.c).
Its copyright and permission notice are retained in the adapted files.
No GPL driver code is incorporated.

Run the independent board/boundary controls from an external object directory:

```sh
cc -std=c11 -Wall -Wextra -Werror -O2 \
  /path/to/src/tools/daemonarchy/tests/apple-aic-layout.c -o apple-aic-layout
./apple-aic-layout
```

The UART, NVMe and timer specifiers come from the published J474s DTB. INFO
register values in the test are fixtures, not captured hardware measurements.
These controls exercise layout and specifier decoding; they do not execute
interrupt delivery, acknowledge actual hardware or prove a booted Mac.
Compile the actual driver in a fresh native FreeBSD ARM64 GENERIC build using
the allocated worker before creating a kernel candidate.

Native IRQ/FIQ/IPI delivery, CPU startup, UART clocks, power/reset providers,
DART/SART, USB/storage and framebuffer handoff still require their own
implementation or verification. In particular, an installer container with a
matching DTB is not a native M2 boot qualification. Keep macOS, FileVault and
Recovery intact while this work remains incomplete.
