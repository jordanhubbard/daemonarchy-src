# Daemonarchy source development

The original FreeBSD tree stays editable in its conventional layout. This
development project currently has three authored leaf contracts, two shared
support/bootstrap contracts and five
target locks. Installed original-source local-runtime execution now passes;
canonical SDK, per-unit package and Standard lifecycle admission remain open.
Native OS and pkg intent are explicit Flavor choices; compiler/runtime
admission and the actual Standard build profile remain separate work.
An executable retained-profile prototype now selects the original native make
targets and original tests. Its native development receipt is scoped to
local runtime, distinct from the still-missing canonical Standard receipt.

Use a development framework candidate with original-source adoption and
declared-target lock verification ([#22](https://github.com/jordanhubbard/literate-ai/issues/22),
[#79](https://github.com/jordanhubbard/literate-ai/issues/79)). Older installed
clients may not recognize the `original` authority stage. Operator installation
awaits the candidate's native and installed qualification; the version string
alone does not establish that qualification.

- [Initial unit boundaries and remaining qualification](user/source-units.md)
- [Authority and acceptance map](architecture/design-traceability.md)
- [Active development queue](roadmap/active-work.md)

`PROJECT.md` owns the complete-tree, per-unit pkg, incremental execution and
installation/recovery requirements. `tools/daemonarchy/README.md` owns the
existing native controller procedures.

`main` tracks the selected FreeBSD upstream. Owned contributions target the
`daemonarchy` integration branch; review branches hold changes until their
required gates pass. Keep upstream-oriented fixes separately reviewable.
Use an explicit `--repo jordanhubbard/daemonarchy-src` for GitHub commands:
the GitHub CLI may otherwise select the fork's FreeBSD upstream automatically.
