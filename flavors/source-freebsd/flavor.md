---
schema: "literate-ai/flavor-markdown@1"
namespace: "daemonarchy-src"
name: "source-freebsd"
version: "1.0.0"
display_name: "Original FreeBSD source target"
primary_axis: "platform.os"
target: "freebsd"
secondary_constraints: []
applicable_capabilities: []
provides:
  - name: "platform.os.freebsd"
    version: "1.0.0"
    contract: null
requires: []
specification_roots:
  - "openspec/spec.md"
authoring_inputs: []
contributions: []
conflicts: []
co_requisites: []
order_before: []
order_after: []
---
# Original FreeBSD source target

Select FreeBSD system semantics for retained source units. Bind architecture,
ABI, native options and actual compiler/runtime resources separately through
the reviewed retained profile and its admitted inputs. This Flavor supplies
no generation command, ambient compiler fallback or execution qualification.
