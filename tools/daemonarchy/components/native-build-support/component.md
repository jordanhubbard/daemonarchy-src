---
namespace: "daemonarchy-src"
version: "1.0.0"
display_name: "Shared original native build support"
profiles:
  - "retained-contract"
sample: false
provides:
  - name: "daemonarchy.src.native-build-support"
    version: "1.0.0"
    interface:
      uri: "interfaces/contract.md"
      pin: null
requires:
  - requirement_id: "bootstrap"
    capability: "daemonarchy.src.native-bootstrap"
    version_range: ">=1,<2"
    dependency_kind: "build"
    optional: false
    constraints: []
authoring_inputs:
  - kind: "specification-to-source-skill"
    uri: "tools/daemonarchy/skills/specification-to-source/original-boundaries/SKILL.md"
workflow_definition:
  uri: "tools/daemonarchy/workflows/original.md"
routing_policy:
  uri: "tools/daemonarchy/routing/original.json"
flavor_slots:
  - slot_id: "os"
    axis: "platform.os"
    cardinality: "exactly-one"
    capability_contract: "daemonarchy.src.native-build-support"
  - slot_id: "package"
    axis: "packaging"
    cardinality: "exactly-one"
    capability_contract: "daemonarchy.src.native-build-support"
entrypoints: []
acceptance_contracts:
  - kind: "acceptance-contract"
    uri: "acceptance.md"
source_dependencies: []
---
# Shared original native build support

Own the selected original share/mk rules, source and test Makefiles and parent Makefile.inc inputs once, together with the reviewed native-profile constructor, stage harness, original-case inventory, test/report configuration and release-policy binding. Preserve original source and tests; the wrapper must execute their real make targets with isolated objects, staging and explicit captured tools.

Bind the native bootstrap closure as a build dependency. Original reports derive from actual Kyua results and exact case listings. No root-required case may be silently authorized as skipped, and no successful command may invent a passing test or acceptance report. Independently verify sealed exports through the framework acceptance owner. Input/options/toolchain changes invalidate affected work, tests and packages. Registration alone awards no execution or package admission.
