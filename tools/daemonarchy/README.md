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
python3 tools/daemonarchy/prepare-worker.py --build-user builder
```

When this feature is selected, enable it once through the privileged bootstrap:

```sh
sudo -n python3 tools/daemonarchy/prepare-worker.py --build-user builder --enable-filemon
python3 tools/daemonarchy/prepare-worker.py --build-user builder
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

## Controller checks

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s tools/daemonarchy -p 'test_*.py' -v
```

These tests use small synthetic fixtures to exercise refusals, byte verification
and actual subprocess timeout/failure behavior. They do not stand in for native
FreeBSD compilation or package acceptance.
