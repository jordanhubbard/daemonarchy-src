# Daemonarchy source tooling changes

## Unreleased

- Add explicit shared build-support and unknown-provenance bootstrap contracts
  for the initial header/parser/utility closure, preserving the original tree.
- Inventory and capture selected native tools, compiler resources, headers,
  linker references, runtime libraries and test resources with bounded drift and
  symlink checks.
- Prepare exact five-owner retained manifests and executable profiles for the
  installed CLI. Use original make targets, validate the complete original ATF
  inventory and retain independent utility expectations outside build inputs.
  Complete native acceptance and package qualification remain pending.
