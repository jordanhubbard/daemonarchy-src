# Daemonarchy source development changes

## Unreleased

- Add initial canonical original-source contracts for the libutil public header,
  number-parser slice and truncate utility, with explicit dependencies, public
  interfaces and original-test/independent-acceptance requirements. Preserve all
  upstream source locations. Contract validation and target locks are separate
  from native execution and per-unit package qualification, which remain open.
- Document original authority, source/build provenance, complete-tree coverage,
  incremental invalidation and VM installation/recovery requirements.
