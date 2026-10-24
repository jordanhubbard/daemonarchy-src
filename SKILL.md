---
name: literate-ai
description: Maintain original FreeBSD source, native unit contracts and evidence in the Daemonarchy source fork without moving or regenerating the upstream tree.
metadata:
  author: "Daemonarchy maintainers <jordanhubbard@users.noreply.github.com>"
---
# Daemonarchy source development

Read `PROJECT.md`, the [documentation index](docs/README.md),
the [source-unit guide](docs/user/source-units.md)
and the [active work](docs/roadmap/active-work.md) before changing unit
boundaries. Native controller procedures remain in
`tools/daemonarchy/README.md`.

The original FreeBSD implementation remains authoritative. Keep `/usr/src`
editable and preserve source locations, native Makefiles, tests and licensing.
Use review branches targeting the owned `daemonarchy` integration branch;
keep upstream `main` synchronized separately. Use an explicit owned repository
for forge commands, as described in the documentation index.
Do not quarantine, delete or regenerate original source during adoption.

Use `litai project validate`, `litai lock` and read-only retained-project planning
to check authored authority. Exact original-source adoption requires its reviewed
plan and acknowledgement. It grants neither execution nor a passing receipt.
Current source contracts are the initial selected units, not complete tree coverage.

Run original tests against newly built outputs and independent acceptance against
sealed products. Bind compiler resources, headers, sysroot, test tools and runtime
libraries explicitly. Installed bootstrap tools have unknown source-build provenance
until independently established. Do not report a copied executable, test report,
catalogue, aggregate build or pkgbase grouping as qualified per-unit packages.

Keep native objects and staging outside source and the live root.
Use only allocated, aligned workers;
the operator desktop is not an aggregate build worker. Preserve failed evidence.
Never change the live kernel, feeds or boot state as a side effect of contract work.
