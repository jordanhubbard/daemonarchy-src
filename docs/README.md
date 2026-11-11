# Daemonarchy source development

The original FreeBSD tree stays editable in its conventional layout. This
development project currently has three authored leaf contracts, two shared
support/bootstrap contracts and five
target locks. Installed original-source local-runtime execution now passes;
canonical SDK, per-unit package and Standard lifecycle admission remain open.
Native OS and pkg intent are explicit Flavor choices; compiler/runtime
admission and the actual Standard build profile remain separate work.
An executable retained-profile prototype now selects the original native make
targets and original tests. Its native development receipt is scoped to
local runtime, distinct from the still-missing canonical Standard receipt.

Use a development framework candidate with original-source adoption and
declared-target lock verification ([#22](https://github.com/jordanhubbard/literate-ai/issues/22),
[#79](https://github.com/jordanhubbard/literate-ai/issues/79)). Older installed
clients may not recognize the `original` authority stage. Operator installation
awaits the candidate's native and installed qualification; the version string
alone does not establish that qualification.

- [Initial unit boundaries and remaining qualification](user/source-units.md)
- [Authority and acceptance map](architecture/design-traceability.md)
- [Active development queue](roadmap/active-work.md)
- [Wired UEFI loader console and packet contract](user/loader-network-debug.md)
- [Installation trace media binding](user/installation-trace-media.md)
- [RTX Spark bring-up and loader topology capture](rtx-spark-bringup.md)
- [Apple AICv2 preparation and qualification limits](apple-aic2.md)

`PROJECT.md` owns the complete-tree, per-unit pkg, incremental execution and
installation/recovery requirements. `tools/daemonarchy/README.md` owns the
existing native controller procedures.

`main` tracks the selected FreeBSD upstream. Owned contributions target the
`daemonarchy` integration branch; review branches hold changes until their
required gates pass. Keep upstream-oriented fixes separately reviewable.
Use an explicit `--repo jordanhubbard/daemonarchy-src` for GitHub commands:
the GitHub CLI may otherwise select the fork's FreeBSD upstream automatically.

- [RTX Spark bring-up and loader topology capture](rtx-spark-bringup.md)
- [BCM2712 SDHCI bootstrap attachment and qualification limits](pi5-sdhci-bootstrap.md)
- [BCM2712 D0 SD provider preparation](pi5-sd-providers.md)
- [BCM2712 reset prerequisites for RP1](pi5-rp1-reset-prerequisites.md)
- [Selected D0 AON GPIO hog ownership](pi5-aon-gpio-hogs.md)
- [Selected BCM2712 MIP0 MSI provider](pi5-mip-msi.md)
- [BCM2712 DMA address-domain policy](pi5-dma-policy.md)
- [BCM2712 translated RAM busdma](pi5-translated-ram-busdma.md)
- [Selected BCM2712 PCIe2 frontend preparation](pi5-pcie2-frontend.md)
- [ACPI xHCI DMA reachability with full system RAM](pi4-acpi-xhci-dma.md)
- [ARM64 direct-map lookup during mapping updates](arm64-dmap-lookup.md)
- [PL011 receive overflow and interrupt progress](pl011-rx-overflow.md)

- [Pinned FreeBSD CI bootstrap and asset custody](freebsd-vm-ci-bootstrap.md)
