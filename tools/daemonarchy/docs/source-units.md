# Initial original-source units

These three authored contracts select a small real dependency closure. They do
not catalogue the whole source tree or constitute execution admission. Keep
discovered, registered, locked, admitted, built, tested, accepted and packaged
counts separate. Unselected inputs remain in their original source locations.
The [authority and acceptance map](design-traceability.md) connects these
boundaries to the complete source-development requirements and missing evidence.

| Unit | Original inputs | Relationships |
| --- | --- | --- |
| libutil-header | `lib/libutil/libutil.h` | Shared public header owned once; all consumers bind its exact bytes. Other declarations remain present, without claiming their implementations. |
| expand-number | `lib/libutil/expand_number.c`, `lib/libutil/expand_number.3`, `lib/libutil/tests/expand_number_test.c` | Build dependency on the header; original signed/unsigned parser implementation and four original ATF cases. This is a parser slice, not the complete libutil library. |
| truncate | `usr.bin/truncate/truncate.c`, `usr.bin/truncate/truncate.1`, `usr.bin/truncate/tests/truncate_test.sh` | Build dependency on the header and runtime dependency on the parser slice; twenty original ATF shell cases. |

The original `usr.bin/truncate/Makefile` declares `LIBADD=util`. A selected parser
slice must use a distinct private library identity and installation prefix; it
must not replace system libutil or claim all its symbols. Retain the original
Makefiles and shared native build rules as explicitly owned build inputs when
forming the executable manifest. These three leaf contracts do not yet own the
complete bootstrap/build/test infrastructure. Missing ownership must refuse
admission rather than assigning unreviewed files to an arbitrary leaf.

Compiler/linker binaries, resource directories, system headers, startup objects,
linker scripts, runtime libraries, ATF/Kyua and any report-producing interpreter
need explicit custody and bootstrap provenance. A copied host SDK has unknown
source-build provenance. Never attribute it to this fork without a recorded build.

Bind native target, ABI, options, flags, packaging and toolchain through selected
Flavors and the retained profile, separately from unit identity. Until those
Flavors, current locks, Standard binding and execution gates exist, these are
authored contracts with no executable or package qualification.
The initial `freebsd` target locks describe only the three contract/dependency
closures. Their empty Flavor selections do not claim a native toolchain,
packaging provider or executable build profile.

The parser cases are `expand_number__ok`, `expand_number__bad`,
`expand_unsigned` and `expand_generic`. Preserve the utility's complete original
inventory; its `deallocate` case requires root. An unprivileged skip does not
establish complete coverage. Use the closed native Kyua database reporter
described in `tools/daemonarchy/README.md` without inventing pass reports.

Independent acceptance must inspect actual file sizes/content preservation and
malformed-input refusal separately from original tests. Changed source, shared
header, dependency, options or compiler must invalidate the affected plan,
actions, receipts and packages. Package source/build support units as well as
runtime products. Preserve license notices and observed dependency metadata.
