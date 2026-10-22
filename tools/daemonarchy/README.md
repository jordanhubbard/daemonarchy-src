# Daemonarchy source builds

This fork tracks FreeBSD-CURRENT for Daemonarchy hardware work. Keep `main`
identical to the selected upstream tip. Work on review branches; record each
upstream base and submit generally applicable changes to FreeBSD separately.
The build controller lives here to remain versioned with the source it builds.

The initial target is native FreeBSD amd64. Linux/macOS can run controller tests
and inspect plans; upstream cross-build support does not qualify native pkgbase,
installer sets, VM boot or hardware acceptance. These commands do not invoke a
Literate AI lifecycle or issue its acceptance receipt. Daemonarchy's retained
lifecycle integration remains a separate gate.

## Selected-unit retained entry point

For the selected header/parser/utility closure, the common entry point is the
installed candidate's `litai build`, using an explicit retained manifest/profile.
The native controller below remains a separate aggregate diagnostic path.
Use the Python belonging to that same installed candidate as `LITAI_PYTHON`;
do not silently replace the operator's default CLI or install system packages.

On an allocated FreeBSD worker, inventory the reviewed bootstrap selection into
fresh external evidence paths, review its observed dependencies and digest, then
capture it into the ignored project-local bootstrap directory:

```sh
"$LITAI_PYTHON" tools/daemonarchy/inventory-bootstrap.py \
  --selection tools/daemonarchy/retained/bootstrap-selection.json \
  --output <fresh-inventory.json> --observation <fresh-observation.json>
"$LITAI_PYTHON" tools/daemonarchy/capture-seed.py \
  --inventory <inventory.json> --inventory-sha256 <reviewed-sha256> \
  --output "$PWD/tools/daemonarchy/retained/bootstrap"
"$LITAI_PYTHON" tools/daemonarchy/prepare-retained-profile.py --name native-v1
litai build --project . --target freebsd \
  --retained-project tools/daemonarchy/retained/generated/native-v1/manifest.json \
  --retained-project-profile tools/daemonarchy/retained/generated/native-v1/profile.json \
  --retained-project-plan --json
```

This last command is a read-only plan. Review its exact authorization identity,
then repeat without `--retained-project-plan` and with `--authorize-retained-project <identity>`,
`--allow-host-execution`, and fresh external `--obj-dir` and `--build-dir` paths.
Keep inputs frozen during execution. Capture stdout/stderr and exit status on
the worker so an SSH observation timeout cannot discard the run's terminal result.
An occupied generated profile name or capture destination is refused. Changed
inputs require a new profile and authorization; a stale plan must fail.

This prototype retains original make compilation, all 24 original ATF cases,
documentation and eleven withheld utility acceptance cases. No skip is authorized.
The privileged `deallocate` case and explicitly unprivileged permission cases
require a reviewed disposable-VM execution configuration. An installed development
run now passes all 24 original cases and eleven independent cases in that
configuration; see the evidence boundary below. Captured bootstrap provenance
remains unknown; canonical SDK admission, selected-unit pkg construction,
complete-tree ownership and release admission remain open.

## Current native evidence boundary

The [development record](../../verification/source-native-development.json)
separates two completed observations over source revision `d98e6c2da757`:

- Installed framework `6e370033` compiles the selected original units, passes all
  24 original cases without skips and matches eleven independent acceptance
  cases, including one declared invalid-input exit of 1. Its admitted receipt
  is explicitly **local-runtime**. All 34 retained records reverify; scoped
  account permissions are restored and no nullfs mounts remain.
- The unprivileged native controller completes world, kernel, pkgbase and
  installer sets with four jobs. All 537 artifacts verify; the source archive
  matches all 114,710 tracked Git members, including 138 symlinks. Read-only
  inspection verifies 524 pkgbase manifests and identifies a six-package
  compiler/sysroot candidate, including both symbolic and hard-link aliases.

Neither observation supplies canonical Standard lifecycle acceptance, an admitted
source-built SDK, per-unit packages, a booted ISO or release authority. The checked-in
controller review remains bound to `d6070ba5`; the `6e370033` execution used a separate
reviewed disposable native project. Do not silently copy its distribution pin into
this checkout or replace the operator's default CLI. Renew the Standard binding
through its reviewed plan/apply transaction when the next candidate is qualified.

## Worker setup

Use a dedicated unprivileged FreeBSD-CURRENT build account with installed Git,
Python 3.12+ and pkg (`/usr/local/sbin/pkg`), the native base toolchain, at least
80 GiB free on the output volume, and 2 GiB available RAM per job (8 GiB minimum).
Allow additional space for retained builds; a full first world/kernel build can
take hours. Use the existing single-slot worker allocation. The protected signer
and physical acceptance machines are not build workers.

Editable operator checkouts may use the conventional `/usr/src` path. Keep
objects and staging outside the source tree, `/usr/obj` and the live root. The controller refuses root execution and installation/publication are not
operations it exposes. It supplies `NO_ROOT`, isolated staging directories,
explicit src.conf, an empty make.conf/src-env.conf and a bounded job count. It
does not read signing keys, modify host services, change a package feed or reboot.

```sh
git clone https://github.com/jordanhubbard/daemonarchy-src.git "$HOME/daemonarchy-src"
cd "$HOME/daemonarchy-src"
git remote add upstream https://github.com/freebsd/freebsd-src.git
git fetch upstream main
git fetch origin feat/source-build-pipeline
git checkout --detach <full-reviewed-source-commit>
mkdir -p "$HOME/daemonarchy-builds"
```

The checkout must remain clean and frozen for the entire build, including pauses
and resumed runs. Do not use a moving branch name as a source selection. Every
source change, kernel profile or build-option change needs a new build directory.
Do not run baseline and diagnostic builds concurrently on a single-slot worker.

## Plan and build

Use the same arguments for planning, running and resuming. Substitute an actual
40-character source commit and an unused absolute output directory:

```sh
python3.12 tools/daemonarchy/build.py plan \
  --revision <full-reviewed-source-commit> \
  --output "$HOME/daemonarchy-builds/baseline-<commit>" --jobs 8

python3.12 tools/daemonarchy/build.py run \
  --revision <full-reviewed-source-commit> \
  --output "$HOME/daemonarchy-builds/baseline-<commit>" --jobs 8
```

The phases are `buildworld`, `buildkernel`, `packages`, `release-obj`, `sets`.
The native upstream `make packages` builds pkgbase packages for world, kernel and
source; `packageworld`/`packagekernel` have different meanings and create release
sets. The release Makefile's `packagesystem` builds `base.txz`, `kernel.txz`,
`lib32.txz`, `src.txz` and MANIFEST using the same objects and configuration.
External ports modules are excluded; rebuild them against this exact source and
kernel ABI before installation tests.

Each phase has a six-hour default deadline (`--phase-timeout` changes it).
Interrupts and deadlines terminate only that phase's process group. Logs and
failed state remain. An existing output directory is refused unless `--resume`
is given and its retained plan matches exactly. Successful phases are skipped
only when their retained logs still match. A failed phase reruns incrementally;
use a fresh output directory if clean rebuild evidence is required.

`--through buildkernel` permits stopping after compilation for inspection.
Resume with the same source/profile/jobs/output and `--resume` to continue through
packaging. Do not edit or remove partial objects, staging or repository files in
a paused build. Build logs are custody checks, not independent verification of
every intermediate object; discard no retained evidence without operator approval.

```sh
python3.12 tools/daemonarchy/build.py status --output <absolute-build-directory>
python3.12 tools/daemonarchy/build.py verify --output <absolute-build-directory>
```

`plan.json` binds the source commit/tree, upstream merge base, profile, config
hashes, package version, environment and exact commands. `result.json` retains
worker/tool versions, phase exit statuses/log hashes and final artifact hashes.
`verify` requires every successful phase and checks the inventoried bytes. These
are unsigned local records, not authenticated supply-chain provenance or a
framework receipt. Verification detects recorded-byte changes; it does not prove
reproducible binaries, boot success or resistance to a malicious build worker.
The inventory includes every set referenced by MANIFEST, including optional
debug and test sets. Missing, duplicate, unsafe or checksum-mismatched references
fail; required base/kernel/lib32/source sets must also be present.
Source and ports archives follow only their command-line staging symlink,
preserve internal tree symlinks, and normalize archive ownership to root:wheel.
This avoids copying external symlink targets into the distributed trees.

## Diagnostic kernel

Use `--profile diagnostic` and a new output directory. This selects
`DAEMONARCHY_DIAGNOSTIC`, which includes GENERIC and compiles in
`VERBOSE_SYSINIT=0`. Startup tracing is available with the one-boot loader setting
`set debug.verbose_sysinit=1`; it is silent by default. This exposes startup
callback boundaries, not per-driver probe entry/exit or pre-network remote logs.
Additional targeted MSI probe instrumentation belongs on a separate reviewed
patch, with its exact commit retained in the diagnostic build.

Baseline and diagnostic package versions include their source commit, source
timestamp and profile so they cannot silently reuse a version. Both are
development candidates. Keep pkgbase's upstream `FreeBSD-*` names to preserve
its dependency relationships. Package-name compatibility does not authorize
installing them from an unqualified or unsigned feed.

## Promotion to Daemonarchy

The installer still uses verified release sets. A completed source build is a
candidate input, not an automatic replacement for its published snapshots.
Before production selection:

1. Verify bytes and inspect package versions/ABI and set contents. Retain world,
   kernel, source, public firmware/module inputs and release manifests together.
2. Boot a disposable VM; validate the matching world/kernel, module loading,
   installation, update and rollback. Keep old boot environments and image bytes.
3. Build a matching poudriere jail and external graphics/Wi-Fi modules from pinned
   ports. Qualify MSI and MacBook changes on the actual machines.
4. Send only reviewed artifact bytes/manifest to the protected signing workflow.
   Qualify boot trust, package/feed signatures and installer checksum admission.
5. Admit the exact result through the qualified Literate AI retained lifecycle
   and Daemonarchy release matrix before changing production selections.

No host installation, signing, upload, release tag or physical acceptance is
implied by `status: built` or `status: bytes-verified`.

## Observe native actions

Metadata is streamed in bounded chunks with at most 1 MiB of in-memory spooling.
Each make record is limited to 64 MiB; the complete observation retains its
16 GiB and 500,000-entry limits. Source-archive filemon records can exceed the
previous 16 MiB per-record limit. Oversized, redirected, replaced or changing
records still refuse capture. Immutable blobs retain the complete record bytes.


`--trace-mode filemon` requests native make metadata with filemon input tracing,
keeps dependency-file updates disabled, and uses a distinct development package
version. It leaves meta verbosity disabled: helpers such as crunchgen parse
quiet make recipe output as structured data. Planning remains portable.
Execution requires an already configured,
accessible `/dev/filemon` and a make binary with filemon support. The controller
does not load modules or change device permissions. Configure worker features
through the declared worker bootstrap/alignment procedure before dispatch.

The optional native worker feature bootstrap is `prepare-worker.py`. On an idle,
dedicated FreeBSD build worker, inspect readiness for its actual unprivileged
build account (replace `builder` below):

```sh
python3.12 tools/daemonarchy/prepare-worker.py --build-user builder
```

When this feature is selected, enable it once through the privileged bootstrap:

```sh
sudo -n python3.12 tools/daemonarchy/prepare-worker.py --build-user builder --enable-filemon
python3.12 tools/daemonarchy/prepare-worker.py --build-user builder
```

Default inspection makes no changes. Explicit enablement loads the installed
`filemon` module and sets `/dev/filemon` to that account and primary group, mode
0600. Root accounts/groups, redirected devices and unexpected make providers
refuse. No package installation, persistent boot configuration or module unload
is performed. Reboot requires inspection and, if absent, the same explicit
bootstrap again. Run normal worker alignment and the feature inspection before
dispatch; success here does not imply tracing/build qualification. Never enable
this feature on the protected signer or a physical acceptance machine.

Retain command/input metadata as immutable content-addressed copies after each
successful phase. Require real filemon data in compilation metadata and preserve
every successful phase's snapshot across resumes. Metadata capture time is part
of the phase timing. Missing, changed, redirected or over-budget observations
must fail rather than silently falling back to command-only tracing. Allow at
least 100 GiB free for a traced build. Raw metadata can contain private worker
paths; keep it outside Git and published assets.

These are native observations for later canonical Component/action admission.
They do not constitute a complete Component graph, receipt, package provenance
or performance improvement. Compiler bootstrap, shared inputs, variants and
independent acceptance still require qualification.

## Capture the bootstrap compiler seed

`capture-seed.py` copies the exact installed files selected by a reviewed
`daemonarchy/bootstrap-toolchain-inventory@1` inventory. Supply its independently
reviewed SHA-256 and a fresh absolute output directory with an existing parent:

```sh
python3.12 tools/daemonarchy/capture-seed.py \
  --inventory /absolute/reviewed-inventory.json \
  --inventory-sha256 <reviewed-64-character-sha256> \
  --output /absolute/isolated-output/compiler-seed
```

Use the declared versioned `python3.12` executable; an unversioned `python3`
command is not required on the operator machine. The source root defaults to `/`; a separate `--source-root` supports captured
test trees. No compiler or captured executable runs. Input budgets are 8 MiB of
inventory metadata, 10,000 entries, 128 MiB per file and 512 MiB of regular-file
bytes. Reads bind exact modes, sizes and hashes, refuse source ancestor links,
special nodes and mutation, and preserve bounded relative symlinks only when
their direct target is also captured. Declared `GROUP` linker-script references
must match the copied script and selected closure. File modes normalize to
0644/0755; live input modes and files remain untouched.

An existing output always refuses. A failed attempt retains its incomplete
directory without `capture.json`; choose a new path on retry and preserve the
failed evidence. A successful `capture.json` records byte acquisition only.
Installed compiler provenance remains `unknown-host-bootstrap`. This helper
does not admit an SDK, infer complete toolchain coverage, issue a LitAI receipt,
build source, install packages or qualify relocation. The copied `tree` must
still enter the framework's normal custody/admission path, with the reviewed
resource directory, sysroot, linker and runtime bindings. Relocated execution
and linker-script resolution remain required before using it as a qualified SDK.

## Preserve original Kyua test outcomes

`report-kyua.py` converts an explicitly selected, closed Kyua schema-v3 SQLite
results file into the original-case JSON consumed by retained-project test
policy. Use the original results database: Kyua's JUnit export represents
expected failures without failure nodes and puts skipped-result reasons into
diagnostic text. That lossy export cannot establish original pass/skip state.

The reviewed inventory has schema `daemonarchy/kyua-case-inventory@1` and a
`cases` array. Every entry binds the exact test-program relative path, original
case name and a sorted unique retained case ID; for example,
`{"program":"expand_number_test","case":"expand_generic","id":"libutil.expand_generic"}`.
Declare the complete selected inventory, including cases requiring root. The
converter refuses missing, extra, duplicate or orphan program/case/result rows.

```sh
python3.12 tools/daemonarchy/report-kyua.py \
  --results /absolute/run/results.db \
  --inventory /absolute/reviewed-cases.json --action original-tests \
  --output /absolute/run/original-report.json \
  --provenance /absolute/run/original-report-origin.json
```

Both outputs must be new files. Conversion reads a bounded immutable private
copy, verifies the known schema and relationships, and leaves the original DB
untouched. Views, triggers, virtual/unknown tables, unsupported schema versions,
SQLite companions/WAL mode, unsafe paths and exceeded limits refuse. Limits are
64 MiB of DB bytes, 8 MiB of inventory, 100,000 cases, 16 MiB of retained report,
and a ten-second/ten-million-step SQL budget.

Only original `passed` results become passes. Failed, broken and expected-failure
results remain non-passing; original state/reason and database, inventory and
report hashes are retained in the provenance companion. Skips preserve their
exact reason and grant no skip permission. In particular, an unprivileged run's
`Requires root privileges` result cannot satisfy complete privileged acceptance.
Run that original case in the disposable privileged acceptance environment.

Exit zero means conversion succeeded, including conversion of non-passing
results. Literate AI's test policy must still reject failed cases, unauthorized
skips and incomplete coverage. Bind the reporter/runtime, original executable,
reviewed inventory, result DB and both reports through the normal locked
execution/output closure. These outputs do not independently grant a lifecycle
receipt, attest that a binary came from source, or qualify the public retained
execution handler. Native integration checks run an actual Kyua plain fixture
and, on an unprivileged FreeBSD worker, actual ATF expected-failure/root-skip
fixtures; those are report-translation evidence rather than original-unit
compilation or complete original-suite acceptance.

## Controller checks

```sh
PYTHONDONTWRITEBYTECODE=1 python3.12 -m unittest discover \
  -s tools/daemonarchy -p 'test_*.py' -v
```

These tests use small synthetic fixtures to exercise refusals, byte verification
and actual subprocess timeout/failure behavior. They do not stand in for native
FreeBSD compilation or package acceptance.

### Qualified framework renewal

The controller review now binds framework `d6070ba5`, whose complete local
regression and installed-wheel qualification pass. Its exact wheel, distribution,
verified dependency closure and reviewed rebind are recorded in
[`verification/standard-d6070ba5.json`](../../verification/standard-d6070ba5.json).
The update preserves the Standard policy and all sixteen required evidence
kinds. The native dependency build completed with exit 0, and its verified
37-distribution closure installs on the build worker and disposable guest. A
separate native source review applies its own recorded rebind plan; authority
and all five locks pass. Installed native focused checks report 29 passes and
one optional source-corpus skip. This is the preceding binding checkpoint.
The later installed `6e370033` local-runtime result is recorded above; canonical
Standard lifecycle acceptance remains required. Existing operator aliases and
active workspace selections have not changed.

### Reviewed secondary-account profile preparation (development)

The stage harness accepts an explicit runtime view, product view, writable fixture
and secondary account together. The profile constructor can bind those resources
with `--secondary-account NAME`, observing an existing account on the native
worker. It creates no account and selects no provisioning endpoint. All original
case IDs and required-user metadata remain unchanged.

This option requires a qualified framework containing the enforcing secondary
access API; the currently recorded `d6070ba5` baseline does not provide that API.
Once the new framework and its operator-private allocation inspector are
qualified, prepare the profile inside the disposable native worker:

```sh
/path/to/qualified/python -B tools/daemonarchy/prepare-retained-profile.py \
  --project "$PWD" --name secondary-original --secondary-account litaitest
```

The generated profile reserves `runtime` for the captured bootstrap tree,
`products` for sealed build exports and `work` for fresh test fixtures. Reviewed
arguments pass these paths and the account into the helper. The test launcher
uses the views for its interpreter, ATF tools, program search and runtime library
paths. Kyua receives the exact unprivileged account and uses the fixture as its
temporary root. New export directories are explicitly readable even under a
private umask; existing and original directory permissions remain intact.

The helper refuses partial, overlapping, redirected or relative view arguments.
Its legacy mode keeps the previous private paths. Eight local stage checks pass,
including explicit Kyua argument/environment wiring with stubbed programs. These
checks do not execute the original native suite, qualify the installed CLI or
admit an SDK or package.

A subsequent [native harness check](../../verification/secondary-harness-native.json)
runs this committed test stage with previously compiled products and the captured
SDK exposed through read-only, nosuid views. All 24 original cases pass without
skips, including privileged and unprivileged cases. Accounts and compiled products
are unchanged; mounts, ACLs and writable fixtures are restored. The check prepares
its shell launcher with the committed launcher-variable function. It does not run
the selected build profile, qualify the installed CLI, admit the compiler SDK or
issue a lifecycle receipt. Those provenance, acceptance and package gates remain
required.

Controller verification of the secondary-account harness changes selects 66
checks: 62 pass and four native-only checks skip in 3.700 seconds. Current source
authority validation and all five committed FreeBSD-target locks pass. This does
not supply a current lifecycle receipt, original-suite qualification or package
admission. The existing SQLite test fixtures emit ResourceWarnings on the
controller Python 3.14 runtime; no checks fail.

## Debugnet pool readiness fixture

Run the sanitized allocator ownership check in a fresh output directory:

```sh
python3 tools/daemonarchy/tests/run-debugnet-mbuf-pools.py \
  --output-directory /var/tmp/debugnet-pool-check
```

The fixture extracts the current production activation/restoration functions and
uses fake zone objects to check absent/partial pools, refused-start retry and full
restoration. It does not compile the complete kernel or establish real driver
readiness. The native [development evidence](../../verification/debugnet-pool-readiness-development.json)
retains the baseline failure separately from changed-kernel build/runtime gates.
