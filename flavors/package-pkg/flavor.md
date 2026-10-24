---
schema: "literate-ai/flavor-markdown@1"
namespace: "literate-ai"
name: "package-pkg"
version: "1.0.0"
display_name: "FreeBSD pkg packaging"
primary_axis: "packaging"
target: "pkg"
secondary_constraints:
  - axis: "platform.os"
    value: "freebsd"
    optional: false
applicable_capabilities: []
provides:
  - name: "package.format.freebsd-pkg"
    version: "1.0.0"
    contract: null
requires: []
specification_roots:
  - "openspec/spec.md"
authoring_inputs:
  - kind: "agent-skill"
    uri: "../../skills/agent/package-artifacts/SKILL.md"
contributions:
  - contribution_id: "freebsd-pkg-provider"
    kind: "packaging"
    merge_operator: "keyed-union"
    slot: "native-package-provider"
    content:
      kind: "packaging-policy"
      uri: "openspec/spec.md"
conflicts: []
co_requisites: []
order_before: []
order_after: []
---
# FreeBSD pkg packaging

Construct a native pkg archive from exact accepted files on a matching FreeBSD
worker. This Flavor does not authorize installation, signing or publication.
