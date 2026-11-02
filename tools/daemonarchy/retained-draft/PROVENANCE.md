# Draft retained profile (whole src tree): provenance and unknowns

DRAFT, unqualified. Nothing here asserts a build, test, acceptance or release result.
Regenerate with `draft-retained-inputs.py` after capturing the bootstrap seed.

## Grounded in the repository and its documents
- Build action: `python3.12 tools/daemonarchy/build.py run --revision <commit> --output
  <dir> --jobs 4`, from `tools/daemonarchy/README.md` "Plan and build"; its phases
  (`buildworld`, `buildkernel`, `packages`, `release-obj`, `sets`) are the action
  inventory. Four jobs follow the DEV-001 source SDK baseline in the daemonarchy
  roadmap (4 jobs, 8 CPUs, 16 GiB). The README's retained src.conf
  (`tools/daemonarchy/src.conf`) and empty make.conf/src-env.conf are supplied by the
  controller itself. `--revision` is the src rebind PR head
  `1924bee0c02ffede04c5002b5f7b46b44814558f`.
- Test action: the controller suite the repository CI runs
  (`python3 -m unittest discover -s tools/daemonarchy -p 'test_*.py'`), 70 case names
  found statically, run through `unittest-report.py`, which records the outcomes it
  observes at run time.
- Toolchain: the project's own reviewed bootstrap procedure
  (`tools/daemonarchy/retained/bootstrap-selection.json`, selection sha256
  `c58aae0f9b83fc9d4e41c5f17d5fdc87a6692c85bb3edf11fe27422c8d92a190`, same as the
  earlier `verification/bootstrap-capture.json`). `inventory-bootstrap.py` then
  `capture-seed.py` ran on mudflap (FreeBSD 16-CURRENT host): 5,177 entries,
  304,543,211 file bytes, inventory sha256
  `d8b170801ea65b1531ae11ef22f39dba0ec8fab7f97cf0a9286d1992355141b7`.
  The seed lives in the git-ignored `tools/daemonarchy/retained/bootstrap`.
- Release policy: the existing tracked `literate.release.json` (not duplicated).
- Dependency closure: no `dependency` members are declared (zero-member closure).

## UNKNOWN, with reason
- Bootstrap provenance: `unknown-host-bootstrap` (the capture's own words); the seed is
  mudflap's installed base, not a source-built SDK.
- Inventory review: `capture-seed.py` requires an independently reviewed inventory
  digest. No independent review happened; the digest was computed by the drafting
  agent and passed through. Treat the capture as unreviewed.
- Whether `build.py` runs from a retained input copy without `.git`, and its
  clean-checkout and `--revision` checks against this tree (which contains these draft
  files and the untracked seed).
- Dependency closure: worker prerequisites (git, pkg, filemon, 80 GiB free space) are
  not captured; whether the seed's `ld-elf.so.1`/libs satisfy `build.py` is unknown.
- Docs action: no whole-tree docs build or check is defined; `echo` placeholder.
- Original upstream tests (ATF/kyua over the whole tree): no document defines a
  whole-tree run or report path; only the 24 selected-unit cases exist. The
  clang-built tests under `tools/daemonarchy/tests` are not in the inventory.
- Accept action and policy: `verification/retained-project-acceptance.json` covers eleven
  selected-utility cases only; no whole-build acceptance policy exists; `echo` placeholder.
- Test outcomes and skip reasons: nothing was run; some controller tests may skip or
  fail off FreeBSD or without a build user.
- How outputs map: `{output_root}` placeholders and relative output names follow the
  earlier selected-unit profile pattern; their semantics for this action were not run.
