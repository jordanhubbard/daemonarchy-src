---
namespace: daemonarchy-src
version: 1.0.0
display_name: Original libutil public header
profiles: ["retained-contract"]
sample: false
provides:
  - name: daemonarchy.src.libutil-header
    version: 1.0.0
    interface:
      uri: interfaces/contract.md
      pin: null
requires: []
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
# Original libutil public header

Own the exact retained `lib/libutil/libutil.h` once for the selected closure.
Preserve every declaration, conditional and license notice, including the
original generic dispatch between signed and unsigned number expansion.
Consumers reference this owner's exact revision rather than copying its authority.
This header contract does not claim implementation of every declared libutil API.

Changes must invalidate all selected consumers. The source/build pkg must retain
the original header, license and provenance and remain distinct from runtime
library packages. Registration alone does not satisfy native acceptance.
