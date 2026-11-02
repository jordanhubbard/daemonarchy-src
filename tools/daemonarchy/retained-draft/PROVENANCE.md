# Draft retained profile (whole src tree): provenance and unknowns

DRAFT, unqualified. Nothing here asserts a build, test, acceptance or release result. The
`authority-reviewed` marker and every `--authorize-*` flag belong to the operator and were
not used. Regenerate on the amd64 builder, from the project root:

1. capture the bootstrap seed (README "Selected-unit retained entry point") into the ignored
   `tools/daemonarchy/retained/bootstrap`, then run
   `python3.12 tools/daemonarchy/retained-draft/review-bootstrap-seed.py --seed tools/daemonarchy/retained/bootstrap`
2. `closure-inventory.py --spec closure-spec.json --output closure-inventory.json`, then
   `stage-closure.py --inventory closure-inventory.json --root .` (untracked `closure/`;
   add it to `.git/info/exclude`)
3. `litai-venv/bin/python tools/daemonarchy/retained-draft/draft-retained-inputs.py --manifest-out /abs/outside/tree/manifest.json`

## Changes from the first draft
- Placeholders removed: no `echo` tool and no `UNKNOWN-placeholder` action remain.
- Tools: `python3.12` and `kyua` from the reviewed seed, `git` from a captured closure
  (`closure/toolchain`, six libraries). The tests need them on PATH: `test_build.py` runs bare
  `git -C ...` and `test_report_kyua.py` skips without `kyua` (and no skip is authorized).
- First action `draft-ambient-preflight` re-observes ten ambient host inputs by digest
  (`/usr/bin/make`, `cc`, `ld.lld`, `/usr/local/sbin/pkg`, `/sbin/sysctl`, `/usr/libexec/atf-sh`,
  host `kyua`, git's compiled-in helper and template trees, the dynamic linker).
  `build.py` calls the first five by absolute path, so the seed's copies are not what it runs.
- Docs: `draft-docs-tree` checks the 33 project-authored Markdown files (`docs/`, root,
  `tools/`, excluding the seed): UTF-8, relative links and images, fragments.
- Accept: `draft-accept-build-verify` runs `build.py verify` on the sealed build through
  `accept-build.py` (which records the result; `verify` itself writes nothing). It is the same
  controller that built, so it is NOT independent acceptance.
- `--revision` now names `2e8e3bc686af` (the base commit of this draft; the old pin was stale).

## Bootstrap capture review (`review-bootstrap-seed.py`, read-only)
Mechanically verified on mudflap against `tools/daemonarchy/retained/bootstrap`:
- `inventory.json` sha256 `d8b170801ea65b1531ae11ef22f39dba0ec8fab7f97cf0a9286d1992355141b7`
  equals the digest in `capture.json`.
- The tree matches the inventory exactly: 5,177 entries (4,832 files, 28 symlinks, 317
  directories); no missing entry, no size/digest/mode/symlink difference, no entry outside the
  inventory except implicit parent directories.
- No drift: every captured file still has its recorded sha256 on the live host.
- Closure: the `ldd` libraries of every captured ELF file are inside the seed, and the dynamic
  linker `/libexec/ld-elf.so.1` is captured. The seed's `python3.12` resolves its stdlib from the
  seed (`sys.prefix` is the seed's `usr/local`) and imports `sqlite3`, `ctypes`, `lzma`, `ssl`,
  `unittest`, `tarfile`, `termios` and the other modules the controller imports.
- No path looks like private key material (checked by name only).
Not satisfied, and not something a script can satisfy:
- `capture-seed.py` requires an INDEPENDENTLY REVIEWED inventory digest. The digest was
  computed by the drafting agent and passed through; no independent human review happened. Treat
  the seed as unreviewed until the operator or a reviewer records a digest.
- Provenance stays `unknown-host-bootstrap`: the seed is mudflap's installed base
  (FreeBSD 16.0-CURRENT, kernel `303b3646270f`) plus `python312` ports files, not a source-built SDK.
- Coverage: the seed carries cc, make, ld.lld and friends but `build.py` uses the host's. It has
  no git, no pkg, no sysctl. `make buildworld` uses the host's whole base system (hundreds of
  commands and libraries by absolute path); only the six files above are identity-pinned. The
  complete host dependency set of the world build is UNKNOWN.

## Root
`build.py` refuses root (`native_preflight`: "use an unprivileged build account") and the
Kyua-privileged test path needs a non-root user, so the src run must be unprivileged. The
retained executor has no general root refusal, but this action cannot use it. (The product and
ports runs need root for poudriere; they cannot share an invocation with this one.)

## UNKNOWN or blocked, with the evidence
- BLOCKED: `build.py run` calls `git rev-parse HEAD` and `git status` in its own checkout
  (`SOURCE = Path(__file__).parents[2]`) and refuses a revision that is not HEAD. A retained input
  snapshot has no `.git` (the manifest excludes it), so the build action cannot pass its own
  clean-checkout gate as written, whatever `--revision` is. Resolving this needs a change to
  `build.py` or to how the snapshot is made, which is a source/authority change not made here.
- `{output_root}/build` must be an unused, make-safe absolute path (`[A-Za-z0-9_./:-]+`) outside
  the source with at least 80 GiB free; the executor's output root location was not observed.
- Original upstream tests (ATF/kyua over the whole tree): no document defines a whole-tree run
  or report path; only the controller suite (70 cases) is inventoried. The clang-built tests
  under `tools/daemonarchy/tests` are not.
- Docs beyond Markdown: the tree has 3,809 manual pages. `mandoc -Tlint -Werror` over all of them
  (read-only, scratch output) prints 823 lines containing ERROR, mostly vendored `contrib/`.
  No baseline or waiver policy exists, so a pass criterion is UNKNOWN and the pages are not an action.
- Acceptance policy: `verification/retained-project-acceptance.json` covers eleven
  selected-utility cases only; a whole-build policy would need executable exports and does not exist.
  No independent whole-build acceptance is defined.
- Test outcomes and skips: nothing was run through the lifecycle. Cases that skip (for example
  `test_report_kyua` privileged/non-privileged cases) would be refused because every
  `skip_reason` in the policy is null.
- `scale_limits` already declared in `literate.project.json` for this tree (merged in #51).
