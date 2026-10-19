---
namespace: daemonarchy-src
version: 1.0.0
display_name: Original truncate utility
profiles: ["retained-contract"]
sample: false
provides:
  - name: daemonarchy.src.truncate
    version: 1.0.0
    interface:
      uri: interfaces/contract.md
      pin: null
requires:
  - requirement_id: public-header
    capability: daemonarchy.src.libutil-header
    version_range: ">=1,<2"
    dependency_kind: build
    optional: false
    constraints: []
  - requirement_id: number-parser
    capability: daemonarchy.src.expand-number
    version_range: ">=1,<2"
    dependency_kind: runtime
    optional: false
    constraints: []
authoring_inputs:
  - kind: specification-to-source-skill
    uri: tools/daemonarchy/skills/specification-to-source/original-boundaries/SKILL.md
workflow_definition:
  uri: tools/daemonarchy/workflows/original.md
routing_policy:
  uri: tools/daemonarchy/routing/original.json
flavor_slots: []
entrypoints: []
acceptance_contracts:
  - kind: acceptance-contract
    uri: acceptance.md
source_dependencies: []
---
# Original truncate utility

Compile the original `usr.bin/truncate/truncate.c`, retaining its command-line,
file sizing, reference-file, rounding, deallocation and refusal semantics.
The native Makefile's `LIBADD=util` is an actual dependency; this selected proof
links the newly built parser slice under a distinct private identity instead
of silently using the host library or replacing system libutil.

Run all twenty original utility ATF cases against the newly built executable.
Keep privileged prerequisites explicit. Package the sealed utility, manual,
provenance and real runtime dependency metadata; source/build products retain
the corresponding original implementation, tests, build inputs and licenses.
This contract does not authorize installing over the live system utility.
