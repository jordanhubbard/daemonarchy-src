# Source development work

### [ ] SRC-UNIT-001 — Qualify original application/library/header composition

- **Owner:** Source unit contracts and retained native lifecycle binding.
- **Parent program:** [Daemonarchy COMP-001](https://github.com/jordanhubbard/daemonarchy/blob/feat/native-release-program/docs/roadmap/freebsd-components.md).
- **Framework issue:** [Retained native lifecycle #22](https://github.com/jordanhubbard/literate-ai/issues/22).
- **Verification defect:** [Committed non-host lock targets #79](https://github.com/jordanhubbard/literate-ai/issues/79).
- **Direction:** Make conventional editable source available through the new
  incremental build/test/package system without retiring original implementation.
- **Conclusion:** Begin with the actual truncate/parser/shared-header closure,
  then grow the same canonical path to complete source and ports coverage.
- **Implementation and evidence:**
  - [x] Keep framework documentation/queue commands on their conventional `docs/`
    paths and land owned changes through an integration branch distinct from
    upstream-tracking `main`. Move only newly authored metadata documents.
  - [x] Validate three authored unit contracts and resolve exact dependency locks.
  - [x] Review and apply original-source authority without moving original files.
  - [x] Establish explicit shared native build-support and external bootstrap
    owners, inventory the selected compiler/resources/headers/runtime/test tools,
    and copy the reviewed bytes through the existing bounded seed capture.
    Bootstrap source-build provenance remains unknown until independently proved.
    - [x] Close observed version-query and linker-detection gaps: retain the
      original version script, parameter header and copyright inputs; bind the
      captured linker explicitly and check the compiler's actual link selection.
      Preserve the first run's warnings and issue no admission from compiler success.
  - [ ] Bind selected inputs, native configuration, compiler and test resources
    through the qualified Standard retained lifecycle.
    - [x] Establish the first Standard binding through a reviewed installed-wheel
      transaction, preserving original adoption authority and all installed
      Standard evidence requirements. The plan identity is
      `sha256:a8369c3fc540e0f8cf6d186eb23c6cc2fa45ab005e5e74e44dd228965dabf14f`;
      the installed distribution identity is
      `sha256:6c341fe24f1c3fbc859620227ea453d122cd842fc73bc5c920a99063e4ad3cdb`.
      The receipt output path is declared, but no receipt exists.
    - [x] Rebind to installed framework revision
      `cffef3238ba48f2c0876270aa4a1aeacce4c86e1` through reviewed plan
      `sha256:094d62225fb9729c981f26def2f1122de9e9e5c1a0759d5e78ac9c4da7ffb501`;
      current distribution identity is
      `sha256:ed899116420edda0c803acf424067f102b6face4870e180104add4da431cb534`.
      Accepted membership and receipt remain stale until rebuild.
    - [x] Rebind the reviewed project to installed framework revision
      `5b9afcd7c9016341620695b9110ec073809d239c` through plan
      `sha256:d8788377ad6f684e11d9b0db310c66d1172ad1399532a07efad12db595c7e771`.
      Distribution identity is
      `sha256:01c193fa8e09bfd5c38eee2d5aaa71594421c1e67840fad46d40b1740c4a8bc9`;
      all sixteen evidence kinds and the Standard policy are preserved.
      Installed project authority and three locks pass; native preview deployment
      and actual retained execution remain open.
    - [x] Rebind the corrected local and native build-worker review projects to
      installed framework `ca604c000d6a2eae1906fe10a6f7f1156bab2b43` through
      the same reviewed plan `sha256:7d14cf0557dd281087dca3d34cabdf41ce44aab7d37fa7f3fbe9f533b5651108`.
      Distribution `sha256:95ff863c311e475172efbca8a7a2fd012d727974368ce84293503804462b8fe0`
      preserves all sixteen evidence kinds and the Standard policy. Source
      authority and five locks pass; the lifecycle receipt remains missing.
    - [x] Rebind the controller review project to qualified framework
      `d6070ba530a743f323e12e23b37b2c7840d49a3b` through reviewed plan
      `sha256:f274b56021aa1800589687b631fe41349fdff5ebf12a058c8dab0fc153bb95a0`.
      [Renewal record](../../verification/standard-d6070ba5.json) binds the
      tested wheel and verified controller dependency closure. All sixteen
      evidence kinds and the Standard policy are preserved. The native verified
      37-distribution closure now installs on the build worker and disposable
      guest. A separate native review at `949a40c88544` applies plan
      `sha256:518ae5edd39c6deddcef78a0a7dffa2d25a7878a06e2c41d735dbbc4569d2e77`;
      source authority and all five locks pass. Installed focused checks
      report 29 passes and one optional skip; no lifecycle receipt is claimed.
    - [x] Select explicit native FreeBSD and pkg Flavors for the three unit locks;
      keep compiler/runtime admission and execution qualification separate.
  - [ ] Compile the original parser and utility and run all four parser cases and
    all twenty utility cases, including privileged deallocation in a disposable VM.
    Preserve the explicit unprivileged-user requirements of `bad_truncate` and
    `cannot_open` and bind any guest privilege switching in reviewed configuration.
    - [ ] Qualify explicit secondary-user resource grants through
      [framework #85](https://github.com/jordanhubbard/literate-ai/issues/85).
      Actual run and output roots are mode `0700`; selecting Kyua's
      `unprivileged_user` alone cannot grant traversal to test programs or
      captured runtime resources. Preserve private defaults and custody checks;
      do not chmod the run broadly or weaken original test requirements.
    - [x] Exercise the retained native products in a disposable FreeBSD guest:
      all 24 exact original cases pass with no skips, including privileged
      deallocation and both unprivileged cases. The existing closed-database
      reporter verifies the original inventory. A manually scoped fixture
      probe restores pre-existing ACLs and leaves account and payload bytes
      unchanged. [Product evidence](https://github.com/jordanhubbard/daemonarchy/blob/feat/native-release-program/verification/original-native-suite-guest-probe.json)
      retains the failed default-deny fixture-inheritance attempt as well.
      Installed framework grants, SDK admission and lifecycle acceptance remain open.
    - [x] Author the exact 24-case Kyua report inventory from the selected original
      registration lists and validate it with the existing report producer.
    - [x] Compare actual newly built ATF program listings with that inventory
      before running the full suites; refuse missing or additional cases.
  - [ ] Independently accept sealed file effects and parser/library behavior.
    - [x] Author eleven typed utility file-effect cases bound to the current
      three Component locks; retain their expectations outside build inputs.
    - [ ] Bind that verifier policy to the executable profile and observe every
      case against the sealed, newly compiled utility. Parser/library acceptance
      and the complete original test inventories remain separate requirements.
  - [ ] Inspect per-unit pkg products, both BOMs, provenance and dependencies.
    - [x] Retain the selected canonical Component specifications and public
      contracts in the input manifest. Profiles v2/v3 omitted those files;
      the package owner correctly refuses missing authored specifications.
      Keep the separate machine-verifier expectations outside build inputs.
      The subsequent local profile has 5,310 members, all five exact required
      specification blobs and current read-only ownership validation. This
      preparation is not package construction or a native execution result.
  - [ ] Measure unchanged builds and separate source/header/options/toolchain
    invalidation; prove relocated installation, update and rollback.

**Partial evidence:** Canonical project validation accepts all three contracts
and the acyclic header/parser/utility graph. Each `freebsd` target lock resolves
through the actual Component catalog. The initial locks selected no Flavors;
explicit FreeBSD and pkg choices now resolve through native OS/package slots
on all three current locks. Project authority and locks verify; all three skills
pass the configured evaluator checks. These choices do not qualify a compiler,
executable build or pkg product.
The typed current-lock reader and explicit target checks pass. The older
repository verifier incorrectly re-plans every lock as `host`; framework #79
tracks this defect. Preserve the declared target rather than rewriting these
locks to obtain a passing gate. The development repair now accepts all three
current locks; installed framework renewal is still required.

The reviewed in-place adoption binds authored source commit
`4b9bb347c132f537fb612e266c880f1b9b3e5439` and plan identity
`sha256:c1960a595185a5ad26d82d2bb93bb442fa870080112fc8e79f7700556d7f62ac`.
It created only the two authority/evidence JSON files under `.literate`,
preserved all tracked source bytes and retained original release authority.
Its record explicitly grants no execution admission.
The owned `daemonarchy` integration branch starts at the selected upstream tip;
the source-development review targets it while `main` remains upstream-tracking.
**Next action in progress:** configure the actual Standard/native profile,
complete selected input ownership and SDK/test custody, then earn execution
admission; no original unit has yet been compiled through this lifecycle.
A direct native make diagnostic at selected source revision
`978fed917ba2b391c3eace661c21a7a65549cb23` compiles the original parser and utility.
The complete original ATF inventory reports 23 passes and one retained
root-required skip (`truncate.deallocate`); permission-error cases run
unprivileged. ELF inspection confirms the parser test uses the privately built
parser archive and no host libutil. The
[exact product-side record](https://github.com/jordanhubbard/daemonarchy/blob/a9838100a25f51a2d153e1c6de92f3980e43f5e6/verification/native-source-original-tests.json)
retains the actual phase commands, timings and per-case outcomes. These
diagnostics are not installed LitAI execution, privileged acceptance, independent
sealed-product acceptance or package qualification. All those requirements
remain open; no unit is admitted by this checklist.

Five current locked contracts now include shared native build support and an
explicit unknown-provenance bootstrap owner. The bounded native inventory and
capture observe 5,476 entries, 306,736,138 file bytes and 161 ELF files; the
[capture summary](../../verification/bootstrap-capture.json) retains exact
selection, inventory and capture-record hashes. Local controller tests report
56 passes and four native-only skips; the native controller suite reports all
60 passing. These checks qualify helper refusal/custody, not product acceptance.

The installed CLI accepts a real 5,266-member retained manifest with five
canonical owners and 5,618 owned input nodes (files and directories). The managed
graph has five units plus the retained-project root. Changing a selected helper
invalidates the earlier plan. The first installed native run has now ended.
Its build, documentation and original-test commands all exited zero, with 23
original passes and one root-required skip. The exact production-policy recheck
refuses `retained.tests.skip-not-authorized: truncate.deallocate`; no acceptance
action was observed and no passing receipt was produced. The
[retained observation](../../verification/retained-run-v2.json) binds the three
verified command observations and test report. The SSH wrapper lost the
top-level CLI exit code; it is unknown rather than inferred from child exits.
Build stderr also exposes missing source-version inputs and fallback linker
feature detection. Their correction and renewed native execution remain open,
along with privileged tests, withheld acceptance and per-unit pkg products.

Large-inventory source-BOM construction exposed quadratic strict-schema
uniqueness validation. Framework [#84](https://github.com/jordanhubbard/literate-ai/issues/84)
owns the reusable repair; retain complete per-file provenance and validation.
No build-executor decision is justified by that unrelated overhead.

The renewed installed `ca604c00` run has a supervised terminal result:
exit 2 after 2,037.584 seconds. All three original commands exited zero with
empty stderr; the version-query and linker warnings did not recur. The same
23 original cases passed, and the required privileged case remained skipped.
The lifecycle correctly refused that unauthorized skip before independent
acceptance or packaging. [Renewed observation](../../verification/retained-run-v3.json)
binds the exact five-unit profile, command objects, report bytes and CLI
terminal hashes. This is a complete failed-run observation, not a passing
receipt or an equivalent-workload performance comparison. The live input
selection remained frozen; a future native profile must also include the
authored specification inputs prepared in the subsequent local profile.

Secondary-account harness integration is prepared locally. The profile constructor
can observe an existing native account and bind optional worker/account/view/fixture
policy without creating accounts or choosing a provisioner. Stage arguments name
the reviewed runtime/product views, fixture and account explicitly. The launcher,
ATF variables, loader paths, Kyua user and temporary root use those bindings; newly
created export directories remain readable under a private umask. Eight local
stage checks pass, including stubbed runner wiring. The recorded `d6070ba5`
framework lacks this API; a newly qualified framework and actual private allocation
inspector are required before installed execution. Original 24-case coverage,
current receipt, SDK provenance and per-unit packages remain open.

The subsequent [native harness check](../../verification/secondary-harness-native.json)
executes the committed original-test stage against the captured SDK and existing
compiled products. All 24 reviewed cases pass without skips, including root and
unprivileged cases. Exact program/body bytes and accounts are unchanged; ACLs,
mounts and writable fixtures are restored. This validates the new stage path,
not a complete selected-profile run or installed framework admission. The
retained database, provenance and report hashes keep that distinction explicit.


Controller verification of the secondary-account harness changes selects 66
checks: 62 pass and four native-only checks skip in 3.700 seconds. Current source
authority validation and all five committed FreeBSD-target locks pass. This does
not supply a current lifecycle receipt, original-suite qualification or package
admission. The existing SQLite test fixtures emit ResourceWarnings on the
controller Python 3.14 runtime; no checks fail.

The [installed native development record](../../verification/source-native-development.json)
now supersedes the pending-execution description above for its explicit scope:
installed `6e370033` completes the selected original build, all 24 original cases
without skips and eleven independent cases. Receipt
`sha256:924264d401159e317059b69c80087ec3a9cd047ca4652a53ca7903fa6c29f559`
is admitted for local-runtime use; all 34 retained records reverify and account,
ACL and mount cleanup is complete. Earlier failed runs remain retained.
The separate raw source baseline completes world/kernel/pkgbase/sets and verifies
537 artifacts plus exact source-archive parity. Its six-package compiler/sysroot
candidate requires faithful symbolic and hard-link custody through framework
[issue #93](https://github.com/jordanhubbard/literate-ai/issues/93).
Canonical Standard lifecycle, SDK admission and per-unit pkg gates stay open.
Next: admit that source-built SDK through its actual producer and renew consumers;
aggregate package groups and copied bootstrap tools cannot satisfy this step.

### [ ] SRC-NULLFS-001 — Honor standalone nullfs comma-separated options

- **Priority:** P2
- **Requirements:** SRC-G5 and SRC-G6
- **Issue:** https://github.com/jordanhubbard/daemonarchy-src/issues/5
- **Observed:** On the official 20261019 FreeBSD 16.0-CURRENT guest, direct
  `mount_nullfs -o ro,nosuid` exits zero but applies neither flag; root writes
  through the alias succeed. The helper currently adds the complete option
  string as one iovec entry, although its manual documents comma-separated
  options. The `mount -t nullfs` frontend correctly applies both flags.
- **Next:** Prepare a separate upstreamable helper correction and native
  regressions for direct lists, repeated options and the frontend. Keep this
  independent of the retained runtime implementation.
- **Current caller:** The read-only-view prototype uses mount(8), verifies kernel
  flags before sharing, rejects writes, retains relative aliases and unmounts
  cleanly. Its thirteen isolated checks pass; full SDK/lifecycle and package
  admission remain open.

### [ ] SRC-DEBUG-001 — Deliver a wired UEFI loader debug console for the next FreeBSD release

- **GitHub issue:** [#6](https://github.com/jordanhubbard/daemonarchy-src/issues/6)
- **Design and native check:** [Wired loader console](../user/loader-network-debug.md).

- **Priority:** P0
- **Owner:** UEFI loader console and standalone networking
- **Direction:** Implement FreeBSD-only remote loader console output, ordinary environment inspection/update and boot control as a release blocker.
- **Conclusion:** Use the existing EFI NIC and SHA-256 foundations; add an opt-in bounded authenticated session, preserve local console and boot behavior, and independently qualify the actual EFI binary. Kernel handoff coverage remains a separate required phase.
- **Depends on:** none
- **Implementation:**
  - [x] Add authenticated bounded packet encoding, exact session and sequence checks, and a controller with retained output.
  - [x] Connect the protocol to an opt-in EFI console with nonblocking network polling, ordinary interpreter input and clean handoff.
  - [ ] Build the actual FreeBSD EFI loader and qualify VM plus physical firmware NICs; preserve local boot without a peer.
- **Evidence:**
  - [x] Native FreeBSD protocol tests reject malformed, forged, wrong-session, replayed and oversized records; valid input and retransmission remain ordered.
  - [x] Initial actual UEFI VM captures show console output, show/set/boot, disconnect/reconnect, absent-NIC/peer behavior and separately confirmed kernel boot.
  - [ ] Physical laptop captures, shared-NIC netboot regressions and injected firmware faults qualify the release candidate.

SRC-DEBUG-001 now has an integrated EFI console and standalone controller. The
actual FreeBSD amd64 stand build passes (107.388 seconds, two jobs); expanded
native packet checks pass independent HMAC/session vectors, mutation, bounds,
roles and replay refusal. A real OVMF UEFI VM passes authenticated show/set,
interface/status, reconnect, invalid input and lost-ack/reflection tests. Remote
boot over TFTP reaches a separately observed FreeBSD login and native SSH.
The same-NIC TFTP probe and 128-packet unrelated UDP pressure check pass; 112
normal RX drops are exposed without losing debug command access. Captures and
failed preceding build/VM launches are retained privately; the
[development record](../../verification/loader-network-development.json) records
source and artifact identities without publishing keys or raw captures.

One private transmit owner retains pending firmware/DMA memory; bounded RX
queues retain ordinary network frames; overflow drops new ordinary frames and
counts them while continuing debug polling. Fresh challenge/session pairing revokes
old input and cancels partial lines; remote input echo is suppressed and local
consoles remain active. Networking closes before the final memory-map capture.
Physical firmware/NIC qualification, shared-NIC netboot regressions, queue pressure
and injected firmware faults remain open. Kernel networking after handoff and
release admission are not earned by this initial VM result.

SRC-DEBUG-001 native fault fixture now includes the actual EFI driver, with
scripted callbacks for timeout/late/error completion, shared transmit, shutdown
quarantine, RX ownership/overflow, receive/allocation failure and bounded polling.
The native address/undefined-behavior checks pass. A preceding failure reproduced
frame-size integer overflow; corrected mode/bounds guards refuse it. FreeBSD
LeakSanitizer is unsupported, so owned RX allocation balance is asserted directly.
Physical firmware behavior, broader shared-NIC/NFS regressions and release
admission remain open; these tests do not replace their acceptance evidence.

The native FreeBSD boot entry now checks that the selected NIC was quiesced by
device cleanup. Failed shutdown quarantines the channel even without a pending
transmit; handoff is refused while it remains active or quarantined. Native
callback assertions cover this guard's rejection/success states. Actual failed
firmware handoff and alternate chain/hypervisor paths still need qualification.

The failure audit reproduced ignored EFI metadata errors reaching final module
copying. `bi_load()` now propagates `bi_load_efi_data()` errors. A native fixture
includes the actual bootinfo implementation and passes scripted map, allocation,
exit/retry and success paths with address/undefined-behavior sanitizers. This
qualifies those logic paths, not arbitrary firmware failures or alternate exec paths.

A larger NFS regression also reproduced fragment loss: the debug demultiplexer
read a noninitial IPv4 fragment's payload as a UDP header. Fragmented packets now
remain with libsa for reassembly. The production-parser fixture first fails on
the old implementation and passes after correction, including under sanitizers.
In a two-FreeBSD-VM private-wire lab, the old EFI stalls on an 8192-byte patterned
NFS file; the corrected EFI loads and lists that exact file with `nfs.read_size=8192`
while the console remains active. The latest EFI also passes adversarial sessions,
shared-NIC TFTP/queue-pressure checks and independent SSH after remote kernel boot.
The NFS-loaded kernel independently reaches a FreeBSD login after local UFS root mounting. Physical
firmware/NICs, alternate handoffs, kernel debug continuity and release admission
remain required.


The loader now exports `netdebug.hwaddr`, `netdebug.local_ip` and
`netdebug.peer_ip` from the actual successful debug attachment, separately from
ordinary `boot.netif.*` metadata. These identify the last successful activation,
not an active kernel transport. Native production-code checks cover all partial
export failures and refusal to retain mixed old/new fields. A renewed native
sanitizer check passes, and the exact EFI binary in an actual two-e1000 OVMF
guest exports the selected second NIC and boot ID through remote boot into
independently observed kernel environment/native interface state.
[Identity evidence](../../verification/loader-network-identity-development.json)
retains the separate failure: a `net0` TFTP request reused the cached `net1`
connection instead of the requested interface. [Issue #11](https://github.com/jordanhubbard/daemonarchy-src/issues/11)
owns distinct device-unit and live debug ownership. The subsequent ownership
checkpoint below records the repair and scoped cross-interface, same-NIC
TFTP/NFS and local recovery renewals. Comparing the unit and closing the old socket alone would
retire the live debug NIC. Physical firmware, early-kernel transport and release
admission remain open.

The device-unit repair must also address the underlying `netif_open()` lifetime:
`netif_select()` returns one static selection object, while every new open resets
all interface-use bits. Distinct live sockets need stable interface ownership,
correct unit-specific network context and safe final release. Add production-code
two-unit/lifetime controls before changing that owner, then repeat the actual
UEFI regression. The current nine-file native loader overlay does not admit the
complete source seed or compiler closure.

The production socket-owner fixture reproduces the original static selection
alias. The repair gives each live physical interface stable storage shared by
matching sockets, with shutdown deferred until final close. Debug holds its own
raw socket; ordinary file access compares driver and unit, refuses a switch while
a file remains open, and decrements its count on close. Ordinary protocol
configuration is restored explicitly without restarting shared hardware. DHCP
requires a complete cached header with matching Ethernet identity before reuse.

The [ownership checkpoint](../../verification/loader-network-ownership-development.json)
records three native sanitizer ownership/cache suites, three renewed native
parser/SNP/bootinfo suites and 62 passing controller cases with four declared
skips. The exact rebuilt EFI passes actual two-e1000 `net0 → net1 → net0` TFTP,
two same-NIC stop/restart cycles with revocation, session/adversarial checks,
queue pressure with 112 drops and continued TFTP, and fragmented 8192-byte NFS
reads plus NFS kernel boot. Four independent SSH observations match the loader
boot IDs and selected MACs. The NFS case retains the debug address as metadata;
it does not establish that address as active kernel networking. All six final
owned guests exit successfully before their deadlines. Failed development and
harness attempts remain retained. The native source seed/compiler are still
unadmitted; physical firmware, early-kernel transport and release acceptance
remain open. Issue #11 is implemented on the draft review, pending integration.

A subsequent production-device fixture reproduces another unit-switch failure:
if the new NIC receives no DHCP/RARP parameters, the old address was still
accepted. The candidate now retires ordinary address/route/server/path/port/MTU
context with the socket, uses that same cleanup on setup failure, and removes
stale published MTU when a newly configured interface omits it. Native sanitizer
controls first reproduce both refusal and MTU failures, then pass failure, clean
retry and existing active-file/lifetime cases. The [context checkpoint](../../verification/loader-network-context-development.json)
now records a successful stand build from a fully verified `983b4315ec01` tree
plus the pinned production change. The new EFI passes actual cross-NIC TFTP,
two same-NIC revoke/restart cycles and small/fragmented NFS plus kernel loading.
Three independent kernel observations match boot and interface identities. All
four owned guests shut down normally. The NFS debug IP remains metadata only.
Controller renewal selects 66 cases: 62 pass and four native-only cases skip.
Compiler/SDK admission, physical firmware and early-kernel transport remain
open; the preceding ownership checkpoint retains its original artifact scope.

The [panic-only NetGDB baseline](../../verification/netgdb-panic-baseline-development.json)
establishes a real e1000 connection after an
operator-triggered panic and independently verifies the loader handoff identity
against the exact retained kernel. Non-panic entry refuses and local continue
resumes the same guest. The first Linux GDB client lacks the FreeBSD ABI and is
retained as a failed client attempt. On peer close the kernel logs the finish
packet but keeps polling. [Issue #12](https://github.com/jordanhubbard/daemonarchy-src/issues/12)
owns the missing `dc_finish_handler` copy in `debugnet_connect()` and the latent
double termination between `netgdb_dbg_getc()` and generic `gdb_getc()`. The repair
now passes the [changed-kernel native recovery checks](../../verification/netgdb-peer-close-development.json).
A clean exact `791ac20a21ad` GENERIC build completes in 609.905 seconds
(610.475 supervised), with matching kernel/full/debug build IDs. Native FreeBSD
GDB 15.1 reads registers, actual panic memory, a panic backtrace and 104 threads.
Orderly detach returns to DDB. A repeated session verifies the full running
version and detaches successfully. A third client exits immediately without a
detach command: captured FINISHED, generic GDB termination and mbuf restoration
lead back to DDB, where local registers and reset work. Non-panic entry still
refuses and local continue resumes the same guest. The first version-output
assertion truncated at 16 elements; its raw failure remains, and the renewed
512-element check passes. Both owned VMs exit zero before deadline; original
disks are retained. Issue #12 is implemented and natively qualified on this draft,
pending integration. Preserve the panic-only and trusted-private-segment limits;
physical NICs, Netdump, authenticated kernel sessions, continuous early-kernel
transport and release acceptance remain open.


The [native Netdump baseline](../../verification/netdump-native-baseline-development.json)
now passes on the same exact `791ac20a21ad` kernel with matching full symbols.
A pinned upstream `netdumpd` receiver builds natively in 0.93 seconds; the owned
VM's controlled panic sends a 165,371,904-byte minidump with valid header parity.
Native `kgdb` reads the full kernel version, panic pointer/string and backtrace,
and enumerates 100 threads. Independent reconstruction of all 116,076 data
messages from the packet capture reproduces the received core's SHA-256 exactly,
with no gaps or overlapping unique data. The receiver exits zero and both bounded
guests stop before deadline with all backing disks read-only. Initial CLI-option
and driver-text harness failures remain retained. This is a private-wire VM
baseline: physical NICs, authenticated kernel sessions, continuous early-kernel
activation, compiler/SDK admission and final release acceptance remain open.

### [ ] SRC-DEBUG-002 — Refuse unready debugnet allocator activation

- **GitHub issue:** [#14](https://github.com/jordanhubbard/daemonarchy-src/issues/14)

- **Priority:** P0
- **Owner:** Kernel debugnet and mbuf pool ownership
- **Direction:** Make earlier panic diagnostics refuse missing reserved pools without corrupting global allocator ownership.
- **Conclusion:** Issue #14 identifies an unconditional allocator swap before readiness. Check every reserved zone before switching, propagate refusal without a finish call, and retain the panic-only restriction.
- **Depends on:** SRC-DEBUG-001
- **Implementation:**
  - [x] Add production-function absence/partial/ready/retry regressions; change mbuf activation and its sole caller; preserve exact previous-kernel evidence.
- **Evidence:**
  - [x] Reproduce the original allocator swap and pass controller/native sanitized function checks.
  - [x] Build the exact changed kernel and qualify direct pre-driver guard refusal plus continued boot.
  - [x] Renew panic NetGDB and Netdump VM recovery on those exact products.
- **Current evidence:** [Pool readiness](../../verification/debugnet-pool-readiness-development.json)
  reproduces allocator-pointer corruption before repair and passes seven absent/partial
  pool cases, retry and complete restoration on macOS and FreeBSD. Fixtures compile
  verbatim production activation/restoration functions around fake zone objects;
  they do not link the kernel or qualify the driver/caller. The separate exact-kernel
  records below qualify real pre-driver refusal and ready-state VM recovery.

The [early kernel checkpoint](../../verification/debugnet-early-kernel-development.json)
now records a clean exact `7a4b58c717bc` GENERIC build in 521.803 supervised
seconds. Kernel/full/debug products share ELF build ID `38821cf580ffed76cdffc9109ee84bd6f5f4d71a`;
all transfer hashes reverify. Boot-time DDB before driver initialization sees
absent reserved pools. A direct call to the real guard returns `ENXIO` and
preserves ten inspected allocator/saved-state words. Registers work, and the same
guest reaches ordinary boot with independent UUID, kernel revision and NIC
verification. Normal shutdown exits zero before deadline; the original image
remains read-only. The first serial disconnect truncated `continue` after its
first character; the same guest resumes after completing that existing command.
The raw observation remains retained. This is direct allocator refusal, not an
early network session. [Ready-state NetGDB renewal](../../verification/debugnet-ready-recovery-development.json)
passes two detach sessions and an abrupt client closure, restoring the allocator
three times and returning to local DDB. [Netdump renewal](../../verification/debugnet-ready-netdump-development.json)
passes on the same exact kernel; native postmortem reads the panic and 100 threads.
All 165,330,944 core bytes reconstructed from the capture match the received dump
hash without gaps or overlaps. Both VM pairs stop before deadline. Continuous
early-kernel networking, authentication, physical NIC and final source/compiler
admission remain open.

### [ ] SRC-DEBUG-003 — Restore iflib debugnet allocator ownership

- **GitHub issue:** [#16](https://github.com/jordanhubbard/daemonarchy-src/issues/16)
- **Priority:** P0
- **Owner:** Kernel iflib debugnet lifecycle
- **Direction:** Restore each RX free-list allocator and exact prior batching policy after debugnet cleanup before attempting resumable debugger support.
- **Conclusion:** Issue #16 reproduces missing DEBUGNET_END restoration and first-free-list-only activation using the verbatim production callback.
- **Depends on:** SRC-DEBUG-002
- **Implementation:**
  - [x] Add saved per-list allocator/context batching state and pair START/END for every free list; retain the panic-only guard.
- **Evidence:**
  - [x] Baseline sanitizer fixture fails batching, zone restoration and multiple-list activation; candidate passes all three cases on macOS and FreeBSD.
  - [x] Build the exact changed kernel and renew NetGDB/Netdump recovery.
  - [ ] Qualify buffer provenance before any non-panic resume change.
- **Evidence:** [Callback ownership record](../../verification/iflib-debugnet-ownership-development.json). Native fixture supervision exits zero in 1.276 seconds; two complete sessions preserve all queue/list pointers and prior batching values 0, 1 and 2. Controller regression selects 66 tests: 62 pass and four native-only checks skip. Kernel patch style checks and Python lint/format pass.
- **Boundary:** Fake zone identities do not qualify ring-buffer provenance or non-panic resume. The existing panic-only restriction remains enforced.

The exact `4f46a741fb2` GENERIC kernel passes with four jobs in fresh objects in
526.747 supervised seconds, under unchanged 2400-second supervision. The dedicated cold checkout was reused
only after process/cwd/argv, mount/export and clean-status checks; its old revision
and index remain retained, and primary refs/HEAD are unchanged. No installation
is performed. [Live iflib recovery](../../verification/iflib-debugnet-live-recovery-development.json)
now passes two detach sessions and an abrupt client exit. Native GDB sees the
reserved allocator during each session; DDB independently verifies the original
receive-list pointer afterward, cleared saved pointer/active state and restored
batching. Initial values are read before activation. Registers after peer closure
and local reset pass; 104 threads are observed. Both VMs stop before deadline.
The real e1000 uses one 2048-byte receive list and prior batching zero; multiple
lists/other batching values remain fixture-only. The [Netdump renewal](../../verification/iflib-debugnet-netdump-development.json)
now passes: native `kgdb` reads the actual panic, exact version, backtrace and 104
threads. All 165,834,752 bytes reconstructed from 116,400 captured data messages
match the received core hash without gaps or overlaps. Both finite guests stop
cleanly. Read-only product sharing and the selected GENERIC-only ESP preserve
retained evidence within the worker reserve. Buffer provenance, safe non-panic
resume and final source/compiler/product admission remain open.
