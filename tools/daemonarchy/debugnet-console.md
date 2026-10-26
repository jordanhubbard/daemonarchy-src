# Diagnostic kernel console prototype

The diagnostic kernel builds `daemonarchy_netconsole.c` with `DEBUGNET` and
`DAEMONARCHY_NETCONSOLE`. It activates only when `netdebug.plain=1` and the
loader has exported `netdebug.hwaddr`, `netdebug.local_ip`, `netdebug.peer_ip`
and `netdebug.boot_id`. The kernel finds a debugnet capable native Ethernet NIC
with the exact exported MAC, starts it with static IPv4 settings, and connects
to the controller on UDP 26003. The controller must be on the same link. The
NIC is exclusively owned by debugnet until root mounts, when the handler
removes the console and releases the NIC to normal networking.

For controller-to-kernel `DEBUGNET_DATA`, the four-byte network-order input
sequence precedes console bytes inside the debugnet payload. FreeBSD debugnet
removes its own packet header before invoking the kernel receive callback.
The kernel uses the inner sequence to discard retransmitted input after a
lost acknowledgement. Console output is buffered in `putc` and sent when
`getc` polls, because `putc` can run inside a critical section.

Run the controller on a trusted, isolated debug network:

```
python3.12 tools/daemonarchy/debugnet-console.py --bind CONTROLLER_IP \
  --boot-id EXACT_LOADER_BOOT_ID --capture /path/to/new-capture.bin
```

For one controller across the firmware and kernel phases, start it before
issuing the loader `boot` command:

```
python3.12 tools/daemonarchy/loader-console.py TARGET --plain \
  --follow-kernel --kernel-transport debugnet --command boot \
  --output /path/to/new-capture.bin --events /path/to/new-events.jsonl
```

This controller binds the debugnet listener before the firmware handoff,
requires a herald with the same boot ID, then keeps capture and stdin active
at the kernel prompt. A production CLI loopback test covers the actual
firmware FINISH to debugnet mountroot exchange. The standalone controller
above is useful for isolated kernel tests. Both reject an unexpected boot ID.
Plain mode provides no peer
authentication or confidentiality. Both the kernel and controller must be
explicitly enabled; do not expose UDP 26003 on an untrusted network.

At the EFI loader, `set netdebug.plain=1` before `netdebug setup` selects the
explicit plain mode. The firmware controller uses
`loader-console.py TARGET --plain`; the packet HMAC uses a public all-zero
protocol key and provides no authentication. This does not auto-start the
kernel controller.

This is an isolated mountroot implementation slice. The default
`loader-console.py --follow-kernel` path expects LDNC kernel frames;
`--kernel-transport debugnet` selects the native debugnet wire framing.
On October 25, a native bhyve VM passed firmware pairing, remote loader
`boot`, same-boot kernel debugnet admission and remote forced-mountroot input.
The minimal VM root has no init; its later panic does not qualify remote
panic control. Earlier boot blocks, predriver panic, post-mount panic,
normal userspace networking after NIC handoff, hard-reset capture, other
native NICs and physical laptop operation remain unqualified.
