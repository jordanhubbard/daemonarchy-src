---
namespace: "daemonarchy-src"
version: "1.0.0"
display_name: "Installed native bootstrap inputs"
profiles:
  - "retained-contract"
sample: false
provides:
  - name: "daemonarchy.src.native-bootstrap"
    version: "1.0.0"
    interface:
      uri: "interfaces/contract.md"
      pin: null
requires: []
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
    capability_contract: "daemonarchy.src.native-bootstrap"
  - slot_id: "package"
    axis: "packaging"
    cardinality: "exactly-one"
    capability_contract: "daemonarchy.src.native-bootstrap"
entrypoints: []
acceptance_contracts:
  - kind: "acceptance-contract"
    uri: "acceptance.md"
source_dependencies: []
---
# Installed native bootstrap inputs

Own the exact selected installed compiler, linker, compiler resources, system headers, startup objects, linker scripts, runtime libraries, shell/make utilities, original test tools and report-producing interpreter. Inventory and capture their observed bytes before planning. Preserve their external bootstrap origin and unknown source-build provenance; do not attribute these binaries to this source fork.

The portable selection and bounded inventory/capture tools define the initial FreeBSD closure. Changed tools, dependencies, sysroot, selection or configuration invalidate consumers. Capturing a seed is not SDK admission, independent native qualification or a source-built compiler. Keep source/build package metadata distinct from runtime products and retain actual provenance gaps.
