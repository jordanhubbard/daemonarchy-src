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
