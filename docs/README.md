# Daemonarchy source development

The original FreeBSD tree stays editable in its conventional layout. This
development project currently has three authored source-unit contracts and
target locks, with no native execution or package admission yet.

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
