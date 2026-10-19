---
namespace: daemonarchy-src
version: 1.0.0
display_name: Original libutil number parser slice
profiles: ["retained-contract"]
sample: false
provides:
  - name: daemonarchy.src.expand-number
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
# Original libutil number parser slice

Compile the retained `lib/libutil/expand_number.c` against the authoritative
shared header. Preserve signed/unsigned parsing, base and binary suffix handling,
overflow/error behavior and original licenses. Do not substitute an installed
host libutil or label this slice as the complete libutil implementation.

Run the four original parser ATF cases against the newly built slice. Use a
private native library/package identity separate from system libutil. Bind its
real native runtime requirements, original source/build inputs, both BOMs and
actual test/acceptance evidence. Changed header, parser, native options or
toolchain must invalidate the affected consumer closure.
