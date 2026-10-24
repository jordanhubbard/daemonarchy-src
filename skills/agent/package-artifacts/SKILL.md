---
name: package-artifacts
description: Plan, construct, and verify exact native packages for tagged Literate AI Components using a selected package.* Flavor. Use for litai package operations, release preparation that requires package evidence, or package-provider metadata generation for pip wheels, Conan, apt, Homebrew, WinGet, or Chocolatey.
metadata:
  author: Literate AI maintainers <literate-ai-maintainers@users.noreply.github.com>
---

# Package artifacts

Treat packaging as a reproducible transformation of one accepted Component artifact
closure. Never use packaging to repair source, tests, binaries, specifications, or SBOMs.

## Preserve the package boundary

1. Resolve one or more selected `package.*` Flavors for every tagged Component.
   Default a Python-capable unpinned Component to `package-pip`; use
   `package-conan` for an explicit cross-language wildcard. Repeated compatible
   selections create separate package plans over the same accepted artifact closure.
2. Run `litai package plan` and inspect the exact Component lock, target, artifact
   graph, specification closure, source and resolved CycloneDX SBOMs, entrypoints,
   runtime requirements, provider Flavor, and packager-tool identity before execution.
3. Detect the selected native tool on PATH. A package command authorizes use of an
   already available tool and writes only beneath OBJ_DIR; it does not authorize a
   system/global install, package installation, registry login, or publication.
4. Generate provider metadata only when deterministic projection is insufficient. If a
   coding CLI is needed, pass the bounded PackagePlan and selected Flavor, inherit the
   command's model scope, admit only declared files, and reject metadata that changes
   product behavior or dependencies.
5. Construct the native package from exact immutable inputs. Include the Component's
   authored specification closure and bind final executable/library products plus both
   CycloneDX SBOMs. Normalize timestamps and ordering where the native format permits it;
   record unavoidable provider variation.
6. Run `litai package verify` without installing or publishing. Inspect native
   metadata and file tables, recompute every digest, and require a PackageResult bound
   to the exact plan. A stale or missing result blocks release preparation.
7. Let `litai release publish --authorize-external-write` own any registry upload,
   repository submission, tap update, package push, signing-service write, or tag push.

## Respect provider compatibility

- `package-zip`: Linux, macOS, and Windows; deterministic ZIPs preserve planned
  payload paths, specifications and both CycloneDX SBOMs without imposing a language
  ecosystem. The built-in packager binds its compression runtime; no external tool
  installation is required.
- `package-pip`: Linux, macOS, and Windows; require a Python package and build wheels.
- `package-conan`: Linux, macOS, and Windows; portable provider selection does not
  erase target, compiler, architecture, runtime, or ABI identities.
- `package-apt`: Linux only.
- `package-brew`: macOS only.
- `package-winget` and `package-chocolatey`: Windows only; they may both be selected
  when the release serves both package-manager communities.
- Reject an incompatible OS/provider pair before metadata generation or native tool use.

## Fail closed

Reject missing specifications, absent accepted products, stale SBOMs, undeclared package
files, changed native tools, output outside OBJ_DIR, install-time execution during
verification, implicit publication, or package bytes that do not match their result.
