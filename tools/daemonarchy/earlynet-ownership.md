# Early kernel transport: ownership and handoff contract

`sys/dev/daemonarchy/earlynet` gives the kernel a bounded, polled network
console before the ordinary NIC driver attaches. It covers the window where
the MSI reset reports stop: CPU and VM startup, device configuration and
driver attach, up to root configuration. The first backends are the VM NICs:
`em` (Intel 8254x, 8086:100e/100f) and legacy `vtnet` (virtio-net 1af4:1000).

## Activation

The transport is inert unless all of these are true:

- the kernel has `options DAEMONARCHY_EARLYNET` (in `DAEMONARCHY_DIAGNOSTIC`),
  or the `daemonarchy_earlynet` module is preloaded;
- the loader environment has `netdebug.plain=1` and `netdebug.early=1`;
- the EFI loader's `netdebug start` exported a complete identity:
  `netdebug.pci` (domain:bus:slot:function), `netdebug.pci_id`
  (vendor:device), `netdebug.hwaddr`, `netdebug.local_ip`,
  `netdebug.peer_ip` and `netdebug.boot_id`.

The PCI function must still report the exported vendor:device and the
network class, and the backend must read back the exported MAC. If any check
fails, the transport stops and releases the device.

## Owners and states

| State | Who owns the function | Native driver may attach |
|---|---|---|
| `UNCLAIMED` | nobody (not enabled) | yes |
| `OWNED` | earlynet: rings, buffers, BAR mapping, bus mastering | no; the placeholder `earlynet` PCI driver holds the device |
| `RELEASED` | nobody: device reset, DMA memory freed, BAR unmapped | yes |
| `FAILED` | earlynet: quiesce did not complete, DMA may continue | **never**: memory is kept and the device stays held |

The ownership rules:

1. Firmware networking ends at `ExitBootServices`. earlynet never calls
   firmware. It starts by resetting the device itself (`CTRL.RST` for em,
   status 0 for virtio), so loader or firmware state is never inherited.
2. While `OWNED`, the placeholder driver claims the exact PCI function with
   `BUS_PROBE_SPECIFIC`. That keeps `em`/`virtio_pci` from attaching while
   earlynet's rings are live; its detach refuses unless the state is
   `RELEASED`.
3. Interrupts stay masked throughout: `IMC` all ones for em, and
   `VRING_AVAIL_F_NO_INTERRUPT` plus ISR reads for virtio. The transport is
   polled only.
4. Every wait is a fixed number of polls: ARP and each message try
   `EARLYNET_RETRIES` × `EARLYNET_ACK_POLLS` × `EARLYNET_POLL_US`. A
   device reset or frame transmit is bounded by `EARLYNET_RESET_POLLS` or
   `EARLYNET_TX_POLLS`. Nothing sleeps, allocates after start or takes
   locks.
5. Console `putc` only buffers. Completed lines are sent from `putc` only
   outside critical sections (`td_critnest == 0`). The debugger's `getc`
   loop flushes everything else, so panic text reaches the controller when
   DDB polls. After one send timeout, line flushing pauses until the
   controller is heard again, so a missing controller costs one bounded
   timeout, not one per line.
6. Handoff runs at `SI_SUB_ROOT_CONF`, `SI_ORDER_FIRST`, in this order:
   - send `netdebug-stage: kernel-early-release` and FINISHED;
   - remove the console;
   - quiesce: reset the device and clear `RDLEN`/`TDLEN` or the queue PFNs;
   - free DMA and unmap the BAR;
   - detach the placeholder and run `device_probe_and_attach()`, so the
     native driver attaches.

   The diagnostic mountroot console (`SI_ORDER_LAST`) then reheralds with
   the same boot ID on the native driver's debugnet methods. A
   `debugnet-console.py --boot-id ID` session follows both, counting the
   change as `reherald`.
7. If quiesce fails, the state becomes `FAILED`. The device is never handed
   to a driver and its DMA memory is never freed.
8. The NetGDB panic-only guard is unchanged. earlynet provides no GDB stub,
   and NetGDB still requires an attached debugnet-capable driver and panic
   state.

## Coverage stated honestly

- **Covered while OWNED:** from the transport's SYSINIT through device
  configuration and driver attach to `SI_SUB_ROOT_CONF`. That is
  `SI_SUB_KMEM` when built in, or `SI_SUB_KLD` when preloaded as a module.
  Panics in this window enter DDB, whose `getc` sends output and accepts
  input.
- **Not covered:**
  - anything before `SI_SUB_KMEM` (locore, `hammer_time`, pmap and early
    console setup);
  - firmware and boot blocks;
  - the gap between release and the mountroot console's herald;
  - a panic without a debugger (`debug.debugger_on_panic=0`), whose
    buffered output is never flushed;
  - physical NICs other than the two VM backends.
- **Hard reset:** the controller's `--last-stage` record keeps the last
  stage and lines received. Silence never establishes a panic or a reset
  cause.

## Evidence

- `tests/run-earlynet-sim.py` compiles the unchanged core and both backends
  against simulated e1000 and virtio-net devices and a scripted controller.
  It covers the handshake, console round trip, lost-ACK retransmission,
  input backpressure, bounded send timeout, degraded line flushing, no
  controller, malformed frames, bad queue geometry, handoff and failed
  quiesce. A device DMA access to released memory aborts the case.
- The native-loader CI job compiles the `daemonarchy_earlynet` module from
  the same sources.
- Native qualification is still required: the `d3-early-em` and
  `d3-early-vtnet` cases in `netdebug-vm-case.py` have not run. They boot
  with `netdebug.early_panic=1`, which panics on purpose during device
  configuration while earlynet holds the NIC.
