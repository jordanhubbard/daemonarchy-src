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

Add `--events NEW_EVENTS.jsonl` to keep a session/fault record and
`--state PRIVATE_STATE.json` to allow a restart. Controller fault handling:

- Short, length-mismatched, unknown-kind, foreign-source and pre-admission
  packets are ignored without ACK and counted.
- Retransmitted kernel output is ACKed but captured once. FreeBSD debugnet
  does not advance its sequence after a timed-out send, so different bytes
  under the last sequence are captured as new output (`sequence_reuse`);
  identical bytes in that case cannot be distinguished from a retransmission.
  A skipped sequence is captured and recorded as `sequence_gap`.
- Input is sent in chunks of at most 255 bytes, because the kernel ring
  refuses anything larger than its 511 free bytes indefinitely. All bytes
  already read from a pipe are queued; each chunk waits for the prior ACK.
- Unacknowledged input is retransmitted every 0.5 seconds up to a bound
  derived from `--timeout`, then the controller exits 1 with
  `input_unacknowledged`. An idle or missing kernel exits 1 with
  `idle_timeout`. A herald with the expected boot ID on an admitted session
  starts a fresh kernel sequence (`reherald`).
- After a controller crash, run it again with the same `--state` and
  `--resume`. The kernel does not reherald, so the state supplies the admitted
  peer, the last captured output sequence and digest, and the next input
  sequence. The state is written before each input is sent, so a resumed
  controller never reuses a sequence the kernel may have consumed. An input
  outstanding at the crash is reported as `unconfirmed_input` and is not
  replayed. The capture is opened for append. Without the state file a
  restarted controller cannot rejoin the session.

Every exit writes a final `controller_exit` record with its outcome and the
fault, duplicate, gap and byte counters. These are loopback controller checks
(`test_debugnet_console.py`); native link loss, kernel send timeout and
restart behavior on the VM NIC remain separate evidence.

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
panic control. A separate static-init VM mounted root and answered 3/3 host
ICMP requests on the ordinary `vtnet` MAC after debugnet FINISHED. Its init
console markers were absent, so full userspace boot/logging remains open.
Earlier boot blocks, predriver panic, post-mount panic, hard-reset capture,
other native NICs and physical laptop operation remain unqualified.
