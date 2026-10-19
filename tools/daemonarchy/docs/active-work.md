# Source development work

### [ ] SRC-UNIT-001 — Qualify original application/library/header composition

- **Owner:** Source unit contracts and retained native lifecycle binding.
- **Parent program:** [Daemonarchy COMP-001](https://github.com/jordanhubbard/daemonarchy/blob/feat/native-release-program/docs/roadmap/freebsd-components.md).
- **Framework issue:** [Retained native lifecycle #22](https://github.com/jordanhubbard/literate-ai/issues/22).
- **Verification defect:** [Committed non-host lock targets #79](https://github.com/jordanhubbard/literate-ai/issues/79).
- **Direction:** Make conventional editable source available through the new
  incremental build/test/package system without retiring original implementation.
- **Conclusion:** Begin with the actual truncate/parser/shared-header closure,
  then grow the same canonical path to complete source and ports coverage.
- **Implementation and evidence:**
  - [x] Validate three authored unit contracts and resolve exact dependency locks.
  - [ ] Review and apply original-source authority without moving original files.
  - [ ] Bind selected inputs, native configuration, compiler and test resources
    through the qualified Standard retained lifecycle.
  - [ ] Compile the original parser and utility and run all four parser cases and
    all twenty utility cases, including privileged deallocation in a disposable VM.
  - [ ] Independently accept sealed file effects and parser/library behavior.
  - [ ] Inspect per-unit pkg products, both BOMs, provenance and dependencies.
  - [ ] Measure unchanged builds and separate source/header/options/toolchain
    invalidation; prove relocated installation, update and rollback.

**Partial evidence:** Canonical project validation accepts all three contracts
and the acyclic header/parser/utility graph. Each `freebsd` target lock resolves
through the actual Component catalog. These contract-only locks select no
native Flavors and do not qualify a compiler, executable build or pkg provider.
The typed current-lock reader and explicit target checks pass. The older
repository verifier incorrectly re-plans every lock as `host`; framework #79
tracks this defect. Preserve the declared target rather than rewriting these
locks to obtain a passing gate.
**Next action in progress:** review in-place original authority after committing
the authored project; native lifecycle admission remains unfinished.
No unit is admitted, built, accepted or package-qualified by this checklist.
