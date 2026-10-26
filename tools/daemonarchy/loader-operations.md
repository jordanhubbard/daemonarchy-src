# Loader operations and last-stage retention

`loader-console.py` turns D1 options into standard loader commands and
runs them, in this order, after pairing and before any `--command`:

| Option | Loader command |
|---|---|
| `--tftp-root DIR` | `set net.static.ip=TARGET` and `set net.static.rootpath=tftp://CONTROLLER:PORT/`; the controller serves `DIR` read-only |
| `--set NAME=VALUE` | `set NAME=VALUE` |
| `--unset NAME` | `unset NAME` |
| `--show NAME` | `show NAME`; the exact output is recorded as `variable_shown` |
| `--kernel PATH` | `unload`, then `load PATH` (a disk path, or `net0:/name` over TFTP) |
| `--module PATH` | `load PATH` |
| `--lua PATH` | `include PATH`, which the loader runs with its own Lua interpreter |
| `--stop` | `netdebug stop`; the console is revoked and the controller exits 0 |

Values containing whitespace, quotes, `$`, `;` or backslashes are refused
before any packet is sent, because the loader tokenizer would reinterpret
them. Each non-`--command` operation is bracketed by echo markers, so its
recorded output never includes earlier queued output.

The loader's net device normally gets its parameters from BOOTP/DHCP. With
`net.static.ip` and `net.static.rootpath` (and optional
`net.static.netmask`) it uses those instead, and refuses partial, malformed
or off-link settings rather than falling back. `tftp://server:port/`
selects a non-standard TFTP port, so the controller does not need port 69.
When the debug console and TFTP share one NIC, `net.static.ip` must be the
console's local address. The served TFTP root refuses writes, traversal,
symlinks out of the root and files over 512 MiB. Each transfer is bounded
by retries and a deadline, and is recorded as `tftp_request`,
`tftp_complete`, `tftp_refused` or `tftp_failed`.

A controller restart is a new pairing: the loader accepts a fresh challenge,
starts a new session and keeps unacknowledged output.

## Last stage

`--last-stage PATH` (both controllers) keeps a JSON record that is rewritten
atomically whenever the received stage changes:

- phase markers sent by the target (`netdebug-stage: efi-loader ...` from
  `netdebug start`, `netdebug-stage: kernel-rootconf ...` from the kernel
  console), with the first one kept as `earliest_stage`;
- controller stages (`firmware_paired`, `firmware_handoff`,
  `kernel_admitted`, `kernel_transport_closed`);
- milestones seen in output (`mountroot_prompt`, `root_mount_attempt`,
  `debugger_entered`, `ddb_prompt`);
- the first `panic:` line, only if one was received, and the last 512
  output bytes.

When the controller ends, `outcome` and `conclusion` are filled in. After
silence (a hard reset, power loss or hang) the conclusion is "no data after
stage X; silence does not establish a panic or reset cause". Firmware and any
boot block that loads the EFI loader carry no transport; the loader marker
states `earlier=uninstrumented`.

Checks: `test_loader_operations.py` (loader emulator over loopback,
TFTP and stage records), `tests/run-loader-static-net.py` (verbatim
`net_parse_rootpath`, `net_static_params` and `tftp_server_port`) and the
staged Lua phase of `tests/run-loader-debug-setup.py`. None of these is EFI
acceptance; the native VM case `d1-loader-ops` covers the real loader.
