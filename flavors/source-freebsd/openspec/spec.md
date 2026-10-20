# Original FreeBSD target policy

### Requirement: Preserve native source semantics

Selected units SHALL preserve original FreeBSD implementation and interface
authority. Source identity SHALL remain separate from target, architecture,
ABI, native options, toolchain and package-format selections.

#### Scenario: Only an OS choice is present

- **WHEN** a unit selects this Flavor without admitted compiler/runtime inputs
- **THEN** native build and execution qualification remain absent

### Requirement: Bind real native execution

The retained lifecycle SHALL bind exact compiler resources, headers, sysroot,
runtime libraries, worker/target observations, original test prerequisites and
independent acceptance. A metadata-only OS selection SHALL NOT supply an SDK,
claim successful compilation, substitute another OS or authorize installation.

#### Scenario: An incompatible worker requests native packaging

- **WHEN** a package provider requires FreeBSD but the actual worker is incompatible
- **THEN** construction refuses before native tool execution
