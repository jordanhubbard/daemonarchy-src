# Diagnostic kernel console prototype

The diagnostic kernel builds `daemonarchy_netconsole.c` with `DEBUGNET` and
`DAEMONARCHY_NETCONSOLE`. It activates only when `netdebug.plain=1` and the
loader has exported `netdebug.hwaddr`, `netdebug.local_ip`, `netdebug.peer_ip`
and `netdebug.boot_id`. The kernel finds a debugnet capable native Ethernet NIC
with the exact exported MAC, starts it with static IPv4 settings, and connects
to the controller on UDP 26003. The controller must be on the same link. The
NIC is exclusively owned by debugnet until root mounts, when the handler
removes the console and releases the NIC to normal networking.

Run the controller on a trusted, isolated debug network:

```
python3.12 tools/daemonarchy/debugnet-console.py --bind CONTROLLER_IP \
  --boot-id EXACT_LOADER_BOOT_ID --capture /path/to/new-capture.bin
```

The controller receives raw console output and sends stdin lines to the
kernel. It rejects an unexpected boot ID. Plain mode provides no peer
authentication or confidentiality. Both the kernel and controller must be
explicitly enabled; do not expose UDP 26003 on an untrusted network.

At the EFI loader, `set netdebug.plain=1` before `netdebug setup` selects the
explicit plain mode. The firmware controller uses
`loader-console.py TARGET --plain`; the packet HMAC uses a public all-zero
protocol key and provides no authentication. This does not auto-start the
kernel controller.

This is an isolated mountroot implementation slice, not a complete network
bring-up claim. The firmware LDNC controller and the kernel debugnet
controller use different wire framing. `loader-console.py --follow-kernel`
expects LDNC kernel frames rather than debugnet framing. A single continuous
controller and same-boot handoff must be implemented and tested. Predriver
panic, post-mount panic, native NIC
support outside debugnet capable devices, VM mountroot interaction, hard reset
capture, and physical laptop operation remain unqualified.
