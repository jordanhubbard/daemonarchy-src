# Initial original-source units

These three authored contracts select a small real dependency closure. They do
not catalogue the whole source tree or constitute execution admission. Keep
discovered, registered, locked, admitted, built, tested, accepted and packaged
counts separate. Unselected inputs remain in their original source locations.
The [authority and acceptance map](../architecture/design-traceability.md) connects these
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
Flavors and the retained profile, separately from unit identity. The unit slots
require the local `source-freebsd` OS policy and the framework's pinned
`package-pkg` provider. The provider is imported at framework revision
`ab15e04e9fb6aadadf26c9a8085640eec275edcc`, including its retained-origin,
live-admission and independent native-inspection requirements.
These choices select OS and package intent, not a compiler or executable build
profile. Standard binding, admitted compiler/runtime inputs, actual execution
and package qualification remain required. The source target Flavor contributes
no generation command or ambient compiler fallback.

The parser cases are `expand_number__ok`, `expand_number__bad`,
`expand_unsigned` and `expand_generic`. Preserve the utility's complete original
inventory; its `deallocate` case requires root. An unprivileged skip does not
establish complete coverage. Use the closed native Kyua database reporter
described in `tools/daemonarchy/README.md` without inventing pass reports.
`bad_truncate` and `cannot_open` require an unprivileged user. The original
Kyua requirements checker rejects these when invoked as root without configured
`unprivileged_user`; bind that selection explicitly in the reviewed guest
configuration. Preserve each case's required-user semantics rather than running
all cases as root or counting a context-related skip as complete coverage.

`tools/daemonarchy/retained/original-cases.json` is the reviewed selection consumed
by the existing Kyua report producer. It declares the four original parser and
twenty original utility registrations as `parser.*` and `truncate.*` report IDs,
using test-program names `expand_number_test` and `truncate_test`. Before a run,
compare the actual newly built ATF listings with this exact selection. The source
registration check and inventory parser validate authoring only; native discovery
and execution remain pending. No selected case has an authorized skip.

Independent acceptance must inspect actual file sizes/content preservation and
malformed-input refusal separately from original tests. Changed source, shared
header, dependency, options or compiler must invalidate the affected plan,
actions, receipts and packages. Package source/build support units as well as
runtime products. Preserve license notices and observed dependency metadata.

### Prepared utility verifier policy

`verification/retained-project-acceptance.json` uses the framework's typed
retained acceptance policy and filesystem cases. Eleven cases declare exact
fixture bytes, expected file lengths/content digests, command results and bounded
resources. They cover prefix preservation, zero-filled growth, relative sizes,
creation, no-create, reference sizes, multiple operands, binary suffixes,
rounding up and invalid-size refusal without changing the fixture.

The policy binds all three current Component lock identities. Its intended
sealed executable is `products/bin/truncate`, with acceptance action
`accept-truncate`. The eventual profile must select that policy identity, declare
the same exact acceptance inventory and include the executable in its build and
package inventories. Use the existing Standard acceptance owner; do not add a
shell runner that independently awards acceptance.

Keep this verifier policy outside the retained input manifest's roots: the
framework withholds independent expectations from build commands and rejects a
policy included among build inputs. Component lock changes require a fresh
review of the policy binding. These are prepared expectations, not observed
results or a test receipt. No case has yet qualified a built utility; parser
acceptance, original ATF suites and VM qualification remain open.
