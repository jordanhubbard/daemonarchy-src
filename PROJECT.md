# Daemonarchy original-source development

This owned FreeBSD source fork supplies the editable `/usr/src` tree for
Daemonarchy. Literate AI must manage exact original inputs, native dependency
composition, incremental build/test actions and per-unit pkg provenance while
preserving upstream implementation authority and conventional source locations.

The development contract version is 1.2.0; it is not a release publication or
the upstream FreeBSD version. The separate Daemonarchy product repository owns
ISO composition and installation/update/rollback acceptance, together with the
owned ports fork at `/usr/ports`.

## Requirements

- **SRC-G1:** Account for every tracked source, build, support, test, vendor,
  license and metadata input through canonical owners. Resolve conditional
  native dependencies for the selected target/options/toolchain. The initial
  three contracts do not establish full-tree coverage.
- **SRC-G2:** Build selected original source with admitted compiler resources,
  headers and sysroot. Retain complete original test outcomes and independent
  acceptance; unauthorized skips and substituted ambient tools must refuse.
- **SRC-G3:** Produce versioned pkg products for every selected compositional
  unit, including source/build units that link into larger runtime products.
  Bind source/build provenance, both BOMs and typed dependencies.
- **SRC-G4:** Measure clean and unchanged runs and invalidate affected actions
  after source, shared header, dependency, options or toolchain changes.
- **SRC-G5:** Prove relocation, installation, update and rollback in disposable
  acceptance VMs before using resulting products in the release ISO closure.
- **SRC-G6:** Preserve original hierarchy, source authority, licensing and the
  normal fork/upstream contribution path. Keep unselected and unqualified units
  visible. Never infer success from catalogue membership or aggregate packages.

See [the unit boundaries](tools/daemonarchy/docs/source-units.md) and
[the active queue](tools/daemonarchy/docs/active-work.md). No native lifecycle
driver, execution admission, package qualification or release receipt is claimed
by this initial project metadata.
