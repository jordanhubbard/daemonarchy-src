# bhyve aborts in `emulate_inout()` when a polling guest is destroyed

Upstream bhyve issue note, observed with the DEBUG-003 VM cases. No
Daemonarchy change is proposed here; the runner treats this abort as a
teardown artifact and records it.

## Observation

`bhyvectl --destroy --vm=NAME` was used on guests stopped at the DDB
`db>` prompt or the kernel `mountroot>` prompt, each with a 16550 UART on
`com1` backed by `nmdm(4)`. bhyve then sometimes exits on SIGABRT instead
of a clean exit status, after logging:

```
Assertion failed: (error == 0), function emulate_inout, file .../usr.sbin/bhyve/amd64/inout.c, line 228.
vm_run error -1, errno 6
```

Seen on daemon.local (FreeBSD 16.0-CURRENT bdcfeac3e1cb, bhyve built from
the 1.3.1 source tree) on October 25 (VM5, guest at `db>`) and on October
26 (`d7-send-timeout` and `d7-malformed`, bhyve status -6). Other
destroyed guests in the same runs exited without it, so the abort is
timing dependent.

## Likely cause

A guest waiting at either prompt polls the UART with `IN` instructions,
so most VM exits are I/O-port exits. In the non-string path of
`emulate_inout()`, the handler result for an `IN` is written back with
`vm_set_register(vcpu, VM_REG_GUEST_RAX, eax)`, followed by
`assert(error == 0)`. If the VM is destroyed between the exit and that
write-back, the ioctl fails (ENXIO, as `vm_run` also reports) and the
assertion aborts the process. The guest is gone either way, but the
abort replaces an orderly exit status and can leave a core file.

## Reproduction

1. Boot any FreeBSD guest whose console is `-l com1,/dev/nmdmNA` and stop
   it at `db>`, for example `sysctl debug.kdb.enter=1`, or boot it with
   a nonexistent `vfs.root.mountfrom` to wait at `mountroot>`.
2. Keep the guest idle at that prompt; it polls the UART.
3. Run `bhyvectl --destroy --vm=NAME` from another shell. Repeat steps 1-3
   a few times.
4. Some runs end with the assertion above and SIGABRT (exit status 134 from
   a shell, -6 from Python's `subprocess`) instead of a normal exit.

## Suggested upstream direction

Treat a register write-back failure after VM destruction like the
`vm_run` failure that follows, and end the vCPU loop without asserting.
For example, return an error from `emulate_inout()` and let the caller
exit when the VM no longer exists. Keep the assertion for every other
errno.
