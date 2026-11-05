# Licenses in daemonarchy-src

This repository is part of the Daemonarchy project (see [`../LICENSE.daemonarchy`](../LICENSE.daemonarchy); the repository map is in HACKING.md of https://github.com/jordanhubbard/daemonarchy). It is a fork of the FreeBSD source tree. FreeBSD's `COPYRIGHT` is untouched; `LICENSE.daemonarchy` is Daemonarchy's statement, and this directory is the index.

Daemonarchy-original work carries the license terms only (BSD-2-Clause), with no copyright notice. Third-party and upstream-derived components keep the upstream notices, which are retained as their licenses require.

This list makes no claim that the licenses of transitive dependencies are represented. It lists the components of this repository and the licenses verified for them, with the evidence for each. Packages and release sets built from this tree carry their own license metadata, which is not reproduced here.

## Rule for files not listed

1. The FreeBSD tree as a compilation: `COPYRIGHT` in the root, copied verbatim to `licenses/FreeBSD-COPYRIGHT.txt`. It states the terms of "the compilation of software known as FreeBSD" (a two-clause BSD text, `Copyright (c) 1992-2026 The FreeBSD Project`) and also reproduces the 4.4BSD terms of the Regents of the University of California and the 1999 advertising-clause withdrawal. It does not replace the license of any file that has its own.
2. A file with its own license header or SPDX tag is under that license. A directory with its own license file (see the tables below) is under the license in that file unless a file says otherwise.
3. The Daemonarchy-original files listed below are BSD-2-Clause.
4. Anything else follows FreeBSD's own conventions as above. This index does not decide the license of an individual file that has neither header nor tag: UNDETERMINED: needs owner review for those (the scan below counts them).

## Daemonarchy-original files (BSD-2-Clause, `licenses/BSD-2-Clause.txt`)

Method: paths changed by the non-merge commits in `freebsd/main..origin/main` of the maintainer's clone (re-run 2026-11-03 on origin/main 14811ad92d6: 82 commits, all by Jordan Hubbard; `freebsd/main` is the local ref at a377d25bd25c). A path is "added" if those commits created it and "modified" if it existed in FreeBSD.

| Path | License | Evidence |
|---|---|---|
| `tools/daemonarchy/` | BSD-2-Clause | Added by those commits. Files carry `SPDX-License-Identifier: BSD-2-Clause` (e.g. `build.py`, `prepare-worker.py`, `tests/*.c`); the `Copyright (c) 2026 Daemonarchy contributors.` lines that some carried were removed from main on the operator's instruction (commit 2eae02f3651). `tools/daemonarchy/retained-draft/` was added to main after the first scan; 8 of its files carry the same SPDX tag. Files without a header are treated as Daemonarchy-original. |
| `verification/` | BSD-2-Clause | Added by those commits (JSON records, no header). |
| Added files outside those two (not present in FreeBSD): `AGENTS.md`, `CHANGELOG.md`, `flavors/package-pkg/flavor.md`, `flavors/package-pkg/openspec/spec.md`, `flavors/source-freebsd/flavor.md`, `flavors/source-freebsd/openspec/spec.md`, `.github/workflows/daemonarchy-source.yml`, `.literate/conversion-authority.json`, `.literate/imports.json`, `.literate/original-adoption.json`, `literate.project.json`, `literate.release.json`, `.literate/repository-lineage.json`, `.literate/repository-parent.json`, `PROJECT.md`, `SKILL.md`, `skills/agent/package-artifacts/SKILL.md`, `stand/efi/loader/netconsole.c`, `stand/efi/loader/netconsole_hardware.c`, `stand/efi/loader/netconsole_proto.c`, `stand/efi/loader/netconsole_proto.h`, `stand/efi/loader/netconsole_report.c`, `stand/efi/loader/netconsole_report.h`, `stand/efi/loader/netconsole_undi.c`, `stand/efi/loader/netconsole_undi.h`, `stand/lua/netdebug.lua`, `sys/amd64/conf/DAEMONARCHY_DIAGNOSTIC`, `tools/boot/test-efi-time.py`, and `docs/` pages added by those commits | BSD-2-Clause | Not present in `freebsd/main`; added by those commits. Source files carry `SPDX-License-Identifier: BSD-2-Clause` in their first lines (checked for every added `.c`, `.h`, `.lua` and `DAEMONARCHY_DIAGNOSTIC` file; `tools/boot/test-efi-time.py` and the workflow `.github/workflows/daemonarchy-source.yml` have no tag); the `Copyright (c) 2026 Daemonarchy contributors.` lines that some also carried were removed from main on the operator's instruction. The documentation and JSON files have no header. |

## FreeBSD files modified by Daemonarchy commits (license unchanged)

These exist in FreeBSD and were edited by Daemonarchy commits. They keep the license in their own headers; the Daemonarchy edits to them are not separately licensed here.

`.gitignore`, `Makefile.inc1`, `README.md`, `release/Makefile`, `sbin/route/route.c`, `sbin/route/route_netlink.c`, `share/man/man4/hidquirk.4`, `stand/common/dev_net.c`, `stand/efi/include/efilib.h`, `stand/efi/libefi/efinet.c`, `stand/efi/libefi/time.c`, `stand/efi/loader/bootinfo.c`, `stand/efi/loader/conf.c`, `stand/efi/loader/efi_main.c`, `stand/efi/loader/Makefile`, `stand/libsa/bootp.c`, `stand/libsa/netif.c`, `stand/libsa/netif.h`, `stand/lua/loader.lua`, `stand/lua/Makefile`, `stand/lua/menu.lua`, `stand/uboot/net.c`, `sys/dev/hid/hidquirk.c`, `sys/dev/hid/hidquirk.h`, `sys/dev/hid/hkbd.c`, `sys/gdb/netgdb.c`, `sys/kern/kern_mbuf.c`, `sys/net/debugnet.c`, `sys/net/iflib.c`, `sys/sys/mbuf.h`

## Areas of the tree

Generated by scanning every tracked file (`git ls-files`, 114872 files at the commit of this change, re-counted 2026-11-03 on main 14811ad92d6) for an `SPDX-License-Identifier:` tag in its first 8 KiB, and by name for license files. Tag strings are counted as written, so an expression such as `Apache-2.0 WITH LLVM-exception` counts once as that string.

| Top-level directory | Files | Files with an SPDX tag | Most frequent tag strings (files) |
|---|---|---|---|
| `(root)/` | 27 | 0 | no SPDX tags |
| `.github/` | 7 | 0 | no SPDX tags |
| `.literate/` | 5 | 0 | no SPDX tags |
| `bin/` | 1157 | 171 | `BSD-3-Clause` (129); `BSD-2-Clause` (39); `BSD-4-Clause` (3) |
| `cddl/` | 2237 | 5 | `BSD-2-Clause` (3); `CDDL 1.0` (2) |
| `contrib/` | 37291 | 16958 | `Apache-2.0 WITH LLVM-exception` (15128); `BSD-2-Clause` (683); `MIT OR Apache-2.0 WITH LLVM-exception` (477); `0BSD` (211) |
| `crypto/` | 10400 | 2 | `MIT` (2) |
| `docs/` | 5 | 0 | no SPDX tags |
| `etc/` | 36 | 0 | no SPDX tags |
| `flavors/` | 4 | 0 | no SPDX tags |
| `include/` | 212 | 159 | `BSD-3-Clause` (77); `BSD-2-Clause` (69); `ISC` (3); `BSD-4-Clause` (3) |
| `kerberos5/` | 125 | 5 | `BSD-2-Clause` (5) |
| `krb5/` | 137 | 110 | `BSD-2-Clause` (109); `BSD-2-Clause OR ISC` (1) |
| `lib/` | 5721 | 1891 | `BSD-2-Clause` (1180); `BSD-3-Clause` (594); `BSD-4-Clause` (33); `ISC` (28) |
| `libexec/` | 584 | 141 | `BSD-2-Clause` (92); `BSD-3-Clause` (36); `BSD-4-Clause` (12); `BSD-2-Clause-FreeBSD` (1) |
| `packages/` | 287 | 131 | `ISC` (115); `BSD-2-Clause` (9); `BSD-3-Clause` (6); `X11` (1) |
| `release/` | 112 | 30 | `ISC` (23); `BSD-2-Clause` (7) |
| `rescue/` | 6 | 0 | no SPDX tags |
| `sbin/` | 1305 | 355 | `BSD-2-Clause` (205); `BSD-3-Clause` (126); `BSD-4-Clause` (16); `BSD-2-Clause-FreeBSD` (3) |
| `secure/` | 1164 | 3 | `BSD-2-Clause` (2); `Unlicense` (1) |
| `share/` | 3845 | 368 | `BSD-2-Clause` (312); `BSD-3-Clause` (22); `ISC` (18); `BSD-4-Clause` (10) |
| `skills/` | 1 | 0 | no SPDX tags |
| `stand/` | 734 | 99 | `BSD-2-Clause` (93); `MIT` (6); `BSD-2-Clause-Patent` (1) |
| `sys/` | 36695 | 24997 | `BSD-2-Clause` (4343); `(GPL-2.0-only OR BSD-2-Clause)` (3620); `CDDL-1.0` (3544); `GPL-2.0` (2135) |
| `targets/` | 58 | 2 | `BSD-2-Clause` (2) |
| `tests/` | 2455 | 423 | `BSD-2-Clause` (404); `BSD-3-Clause` (12); `ISC` (4); `BSD-2-Clause-FreeBSD` (2) |
| `tools/` | 4016 | 511 | `BSD-2-Clause` (495); `BSD-3-Clause` (8); `BSD-4-Clause` (5); `Beerware` (1) |
| `usr.bin/` | 3327 | 640 | `BSD-3-Clause` (313); `BSD-2-Clause` (273); `BSD-4-Clause` (33); `Apache-2.0 WITH LLVM-exception` (13) |
| `usr.sbin/` | 2897 | 1053 | `BSD-2-Clause` (820); `BSD-3-Clause` (150); `BSD-4-Clause` (74); `BSD-4.3TAHOE` (3) |
| `verification/` | 22 | 0 | no SPDX tags |

### contrib/ and sys/contrib/ components

| Component | Files | Tagged | Tag strings (top 3) | In-tree license files |
|---|---|---|---|---|
| `contrib/arm-optimized-routines/` | 491 | 480 | `MIT OR Apache-2.0 WITH LLVM-exception` (477); `MIT OR Apache-2.0 WITH LLVM-exception.` (3) | `contrib/arm-optimized-routines/LICENSE` copied as `licenses/in-tree/contrib__arm-optimized-routines__LICENSE` |
| `contrib/atf/` | 146 | 1 | `BSD-2-Clause` (1) | `contrib/atf/COPYING` copied as `licenses/in-tree/contrib__atf__COPYING` |
| `contrib/bc/` | 1093 | 135 | `BSD-2-Clause` (135) | `contrib/bc/LICENSE.md` copied as `licenses/in-tree/contrib__bc__LICENSE.md`<br>`contrib/bc/NOTICE.md` copied as `licenses/in-tree/contrib__bc__NOTICE.md` |
| `contrib/bearssl/` | 466 | 0 | no SPDX tags | `contrib/bearssl/LICENSE.txt` copied as `licenses/in-tree/contrib__bearssl__LICENSE.txt` |
| `contrib/bionic-x86_64-string/` | 14 | 0 | no SPDX tags | none found by name |
| `contrib/blocklist/` | 75 | 0 | no SPDX tags | none found by name |
| `contrib/bmake/` | 983 | 70 | `BSD-2-Clause` (69); `BSD-3-Clause` (1) | `contrib/bmake/LICENSE` copied as `licenses/in-tree/contrib__bmake__LICENSE` |
| `contrib/bsddialog/` | 70 | 58 | `CC0-1.0` (38); `BSD-2-Clause` (20) | `contrib/bsddialog/LICENSE` copied as `licenses/in-tree/contrib__bsddialog__LICENSE` |
| `contrib/bsnmp/` | 85 | 0 | no SPDX tags | none found by name |
| `contrib/byacc/` | 751 | 0 | no SPDX tags | `contrib/byacc/LICENSE` copied as `licenses/in-tree/contrib__byacc__LICENSE` |
| `contrib/bzip2/` | 31 | 0 | no SPDX tags | `contrib/bzip2/LICENSE` copied as `licenses/in-tree/contrib__bzip2__LICENSE` |
| `contrib/com_err/` | 14 | 0 | no SPDX tags | none found by name |
| `contrib/dma/` | 28 | 0 | no SPDX tags | `contrib/dma/LICENSE` copied as `licenses/in-tree/contrib__dma__LICENSE` |
| `contrib/ee/` | 13 | 0 | no SPDX tags | none found by name |
| `contrib/elftoolchain/` | 441 | 4 | `BSD-2-Clause` (3); `BSD-2-Clause-FreeBSD` (1) | none found by name |
| `contrib/expat/` | 127 | 86 | `MIT` (85); `CC0-1.0` (1) | `contrib/expat/COPYING` copied as `licenses/in-tree/contrib__expat__COPYING` |
| `contrib/file/` | 645 | 0 | no SPDX tags | `contrib/file/COPYING` copied as `licenses/in-tree/contrib__file__COPYING`<br>`contrib/file/python/LICENSE` copied as `licenses/in-tree/contrib__file__python__LICENSE` |
| `contrib/flex/` | 37 | 0 | no SPDX tags | `contrib/flex/COPYING` copied as `licenses/in-tree/contrib__flex__COPYING` |
| `contrib/gdtoa/` | 53 | 0 | no SPDX tags | none found by name |
| `contrib/googletest/` | 247 | 0 | no SPDX tags | `contrib/googletest/LICENSE` copied as `licenses/in-tree/contrib__googletest__LICENSE` |
| `contrib/hyperv/` | 9 | 0 | no SPDX tags | none found by name |
| `contrib/jemalloc/` | 240 | 0 | no SPDX tags | `contrib/jemalloc/COPYING` copied as `licenses/in-tree/contrib__jemalloc__COPYING` |
| `contrib/kyua/` | 535 | 0 | no SPDX tags | `contrib/kyua/LICENSE` copied as `licenses/in-tree/contrib__kyua__LICENSE` |
| `contrib/ldns/` | 138 | 0 | no SPDX tags | `contrib/ldns/LICENSE` copied as `licenses/in-tree/contrib__ldns__LICENSE` |
| `contrib/ldns-host/` | 3 | 0 | no SPDX tags | none found by name |
| `contrib/less/` | 73 | 0 | no SPDX tags | `contrib/less/LICENSE` copied as `licenses/in-tree/contrib__less__LICENSE` |
| `contrib/lib9p/` | 46 | 0 | no SPDX tags | `contrib/lib9p/COPYRIGHT` copied as `licenses/in-tree/contrib__lib9p__COPYRIGHT` |
| `contrib/libarchive/` | 1385 | 216 | `BSD-2-Clause` (210); `BSD-3-Clause` (6) | `contrib/libarchive/COPYING` copied as `licenses/in-tree/contrib__libarchive__COPYING` |
| `contrib/libbegemot/` | 3 | 0 | no SPDX tags | none found by name |
| `contrib/libc-pwcache/` | 3 | 0 | no SPDX tags | none found by name |
| `contrib/libc-vis/` | 5 | 0 | no SPDX tags | none found by name |
| `contrib/libcbor/` | 180 | 1 | `(MIT OR CC0-1.0)` (1) | `contrib/libcbor/LICENSE.md` copied as `licenses/in-tree/contrib__libcbor__LICENSE.md` |
| `contrib/libcxxrt/` | 21 | 0 | no SPDX tags | `contrib/libcxxrt/LICENSE` copied as `licenses/in-tree/contrib__libcxxrt__LICENSE` |
| `contrib/libder/` | 36 | 17 | `BSD-2-Clause` (17) | `contrib/libder/LICENSE` copied as `licenses/in-tree/contrib__libder__LICENSE` |
| `contrib/libdiff/` | 169 | 0 | no SPDX tags | `contrib/libdiff/LICENCE` copied as `licenses/in-tree/contrib__libdiff__LICENCE` |
| `contrib/libdivsufsort/` | 10 | 0 | no SPDX tags | `contrib/libdivsufsort/LICENSE` copied as `licenses/in-tree/contrib__libdivsufsort__LICENSE` |
| `contrib/libedit/` | 61 | 0 | no SPDX tags | none found by name |
| `contrib/libevent/` | 222 | 0 | no SPDX tags | `contrib/libevent/LICENSE` copied as `licenses/in-tree/contrib__libevent__LICENSE`<br>`contrib/libevent/cmake/COPYING-CMAKE-SCRIPTS` copied as `licenses/in-tree/contrib__libevent__cmake__COPYING-CMAKE-SCRIPTS`<br>`contrib/libevent/cmake/Copyright.txt` copied as `licenses/in-tree/contrib__libevent__cmake__Copyright.txt` |
| `contrib/libexecinfo/` | 6 | 0 | no SPDX tags | none found by name |
| `contrib/libfido2/` | 255 | 206 | `BSD-2-Clause` (206); `BSD-2-Clause"` (1) | `contrib/libfido2/LICENSE` copied as `licenses/in-tree/contrib__libfido2__LICENSE` |
| `contrib/libpcap/` | 299 | 0 | no SPDX tags | `contrib/libpcap/LICENSE` copied as `licenses/in-tree/contrib__libpcap__LICENSE` |
| `contrib/libsamplerate/` | 10 | 0 | no SPDX tags | `contrib/libsamplerate/COPYING` copied as `licenses/in-tree/contrib__libsamplerate__COPYING` |
| `contrib/libucl/` | 143 | 0 | no SPDX tags | `contrib/libucl/COPYING` copied as `licenses/in-tree/contrib__libucl__COPYING` |
| `contrib/libxo/` | 429 | 0 | no SPDX tags | `contrib/libxo/Copyright` copied as `licenses/in-tree/contrib__libxo__Copyright`<br>`contrib/libxo/LICENSE` copied as `licenses/in-tree/contrib__libxo__LICENSE` |
| `contrib/libyaml/` | 61 | 0 | no SPDX tags | `contrib/libyaml/License` copied as `licenses/in-tree/contrib__libyaml__License` |
| `contrib/llvm-project/` | 15765 | 15128 | `Apache-2.0 WITH LLVM-exception` (15128) | `contrib/llvm-project/clang/LICENSE.TXT` copied as `licenses/in-tree/contrib__llvm-project__clang__LICENSE.TXT`<br>`contrib/llvm-project/compiler-rt/LICENSE.TXT` copied as `licenses/in-tree/contrib__llvm-project__compiler-rt__LICENSE.TXT`<br>`contrib/llvm-project/libcxx/LICENSE.TXT` copied as `licenses/in-tree/contrib__llvm-project__libcxx__LICENSE.TXT`<br>`contrib/llvm-project/libunwind/LICENSE.TXT` copied as `licenses/in-tree/contrib__llvm-project__libunwind__LICENSE.TXT`<br>`contrib/llvm-project/lld/LICENSE.TXT` copied as `licenses/in-tree/contrib__llvm-project__lld__LICENSE.TXT`<br>`contrib/llvm-project/lldb/LICENSE.TXT` copied as `licenses/in-tree/contrib__llvm-project__lldb__LICENSE.TXT`<br>`contrib/llvm-project/llvm/LICENSE.TXT` copied as `licenses/in-tree/contrib__llvm-project__llvm__LICENSE.TXT`<br>`contrib/llvm-project/llvm/lib/Support/BLAKE3/LICENSE` copied as `licenses/in-tree/contrib__llvm-project__llvm__lib__Support__BLAKE3__LICENSE`<br>`contrib/llvm-project/llvm/lib/Support/COPYRIGHT.regex` copied as `licenses/in-tree/contrib__llvm-project__llvm__lib__Support__COPYRIGHT.regex`<br>`contrib/llvm-project/openmp/LICENSE.TXT` copied as `licenses/in-tree/contrib__llvm-project__openmp__LICENSE.TXT`<br>`contrib/llvm-project/openmp/runtime/src/thirdparty/ittnotify/LICENSE.txt` copied as `licenses/in-tree/contrib__llvm-project__openmp__runtime__src__thirdparty__ittnotify__LICENSE.txt` |
| `contrib/lua/` | 78 | 0 | no SPDX tags | none found by name |
| `contrib/lutok/` | 208 | 0 | no SPDX tags | `contrib/lutok/COPYING` copied as `licenses/in-tree/contrib__lutok__COPYING` |
| `contrib/lyaml/` | 31 | 0 | no SPDX tags | `contrib/lyaml/LICENSE` copied as `licenses/in-tree/contrib__lyaml__LICENSE` |
| `contrib/mandoc/` | 201 | 0 | no SPDX tags | `contrib/mandoc/LICENSE` copied as `licenses/in-tree/contrib__mandoc__LICENSE` |
| `contrib/mknod/` | 2 | 0 | no SPDX tags | none found by name |
| `contrib/mtree/` | 15 | 0 | no SPDX tags | none found by name |
| `contrib/ncurses/` | 595 | 0 | no SPDX tags | `contrib/ncurses/COPYING` copied as `licenses/in-tree/contrib__ncurses__COPYING` |
| `contrib/netbsd-tests/` | 1946 | 0 | no SPDX tags | none found by name |
| `contrib/netcat/` | 7 | 0 | no SPDX tags | none found by name |
| `contrib/ntp/` | 1476 | 1 | `BSD-3-Clause` (1) | `contrib/ntp/sntp/libevent/LICENSE` copied as `licenses/in-tree/contrib__libevent__LICENSE`<br>`contrib/ntp/sntp/libevent/cmake/COPYING-CMAKE-SCRIPTS` copied as `licenses/in-tree/contrib__libevent__cmake__COPYING-CMAKE-SCRIPTS`<br>`contrib/ntp/sntp/libevent/cmake/Copyright.txt` copied as `licenses/in-tree/contrib__libevent__cmake__Copyright.txt`<br>`contrib/ntp/COPYRIGHT` copied as `licenses/in-tree/contrib__ntp__COPYRIGHT`<br>`contrib/ntp/sntp/COPYRIGHT` copied as `licenses/in-tree/contrib__ntp__COPYRIGHT`<br>`contrib/ntp/libjsmn/LICENSE` copied as `licenses/in-tree/contrib__ntp__libjsmn__LICENSE`<br>`contrib/ntp/sntp/libopts/COPYING.gplv3` copied as `licenses/in-tree/contrib__ntp__sntp__libopts__COPYING.gplv3`<br>`contrib/ntp/sntp/libopts/COPYING.lgplv3` copied as `licenses/in-tree/contrib__ntp__sntp__libopts__COPYING.lgplv3`<br>`contrib/ntp/sntp/libopts/COPYING.mbsd` copied as `licenses/in-tree/contrib__ntp__sntp__libopts__COPYING.mbsd` |
| `contrib/nvi/` | 186 | 0 | no SPDX tags | `contrib/nvi/LICENSE` copied as `licenses/in-tree/contrib__nvi__LICENSE`<br>`contrib/nvi/regex/COPYRIGHT` copied as `licenses/in-tree/contrib__nvi__regex__COPYRIGHT` |
| `contrib/ofed/` | 629 | 14 | `GPL-2.0 or Linux-OpenIB` (13); `(GPL-2.0 WITH Linux-syscall-note) OR Linux-OpenIB)` (1) | none found by name |
| `contrib/one-true-awk/` | 418 | 0 | no SPDX tags | `contrib/one-true-awk/LICENSE` copied as `licenses/in-tree/contrib__one-true-awk__LICENSE` |
| `contrib/openbsm/` | 273 | 0 | no SPDX tags | `contrib/openbsm/LICENSE` copied as `licenses/in-tree/contrib__openbsm__LICENSE` |
| `contrib/opencsd/` | 146 | 0 | no SPDX tags | none found by name |
| `contrib/openpam/` | 230 | 0 | no SPDX tags | `contrib/openpam/LICENSE` copied as `licenses/in-tree/contrib__openpam__LICENSE` |
| `contrib/openresolv/` | 20 | 0 | no SPDX tags | `contrib/openresolv/LICENSE` copied as `licenses/in-tree/contrib__openresolv__LICENSE` |
| `contrib/pam-krb5/` | 254 | 223 | `BSD-3-clause or GPL-1+` (141); `MIT` (46); `FSFAP` (25) | `contrib/pam-krb5/LICENSE` copied as `licenses/in-tree/contrib__pam-krb5__LICENSE` |
| `contrib/pam_modules/` | 12 | 0 | no SPDX tags | `contrib/pam_modules/pam_passwdqc/LICENSE` copied as `licenses/in-tree/contrib__pam_modules__pam_passwdqc__LICENSE` |
| `contrib/pf/` | 30 | 0 | no SPDX tags | none found by name |
| `contrib/pjdfstest/` | 247 | 0 | no SPDX tags | `contrib/pjdfstest/COPYING` copied as `licenses/in-tree/contrib__pjdfstest__COPYING`<br>`contrib/pjdfstest/LICENSE` copied as `licenses/in-tree/contrib__pjdfstest__COPYING` |
| `contrib/pkgconf/` | 652 | 75 | `pkgconf` (59); `BSD-2-Clause` (15); `LicenseRef-scancode-sudo AND BSD-4-Clause` (1) | `contrib/pkgconf/COPYING` copied as `licenses/in-tree/contrib__pkgconf__COPYING` |
| `contrib/pnglite/` | 4 | 0 | no SPDX tags | `contrib/pnglite/LICENSE` copied as `licenses/in-tree/contrib__pnglite__LICENSE` |
| `contrib/pnpinfo/` | 4 | 0 | no SPDX tags | none found by name |
| `contrib/processor-trace/` | 100 | 0 | no SPDX tags | none found by name |
| `contrib/sendmail/` | 610 | 0 | no SPDX tags | `contrib/sendmail/LICENSE` copied as `licenses/in-tree/contrib__sendmail__LICENSE` |
| `contrib/smart/` | 12 | 1 | `BSD-2-Clause` (1) | `contrib/smart/LICENSE` copied as `licenses/in-tree/contrib__smart__LICENSE` |
| `contrib/smbfs/` | 50 | 0 | no SPDX tags | `contrib/smbfs/COPYRIGHT` copied as `licenses/in-tree/contrib__smbfs__COPYRIGHT` |
| `contrib/spleen/` | 10 | 6 | `BSD-2-Clause` (6) | `contrib/spleen/LICENSE` copied as `licenses/in-tree/contrib__spleen__LICENSE` |
| `contrib/sqlite3/` | 49 | 0 | no SPDX tags | `contrib/sqlite3/autosetup/LICENSE` copied as `licenses/in-tree/contrib__sqlite3__autosetup__LICENSE`<br>`contrib/sqlite3/tea/license.terms` copied as `licenses/in-tree/contrib__sqlite3__tea__license.terms` |
| `contrib/tcp_wrappers/` | 56 | 0 | no SPDX tags | none found by name |
| `contrib/tcpdump/` | 283 | 0 | no SPDX tags | `contrib/tcpdump/LICENSE` copied as `licenses/in-tree/contrib__libpcap__LICENSE` |
| `contrib/tcsh/` | 434 | 0 | no SPDX tags | none found by name |
| `contrib/telnet/` | 39 | 0 | no SPDX tags | none found by name |
| `contrib/terminus/` | 22 | 0 | no SPDX tags | none found by name |
| `contrib/tnftp/` | 27 | 0 | no SPDX tags | `contrib/tnftp/COPYING` copied as `licenses/in-tree/contrib__tnftp__COPYING` |
| `contrib/ts/` | 2 | 0 | no SPDX tags | none found by name |
| `contrib/tzcode/` | 35 | 0 | no SPDX tags | `contrib/tzcode/LICENSE` copied as `licenses/in-tree/contrib__tzcode__LICENSE` |
| `contrib/tzdata/` | 31 | 0 | no SPDX tags | `contrib/tzdata/LICENSE` copied as `licenses/in-tree/contrib__tzcode__LICENSE` |
| `contrib/unbound/` | 373 | 0 | no SPDX tags | `contrib/unbound/LICENSE` copied as `licenses/in-tree/contrib__unbound__LICENSE`<br>`contrib/unbound/doc/LICENSE` copied as `licenses/in-tree/contrib__unbound__LICENSE` |
| `contrib/unifdef/` | 442 | 0 | no SPDX tags | `contrib/unifdef/COPYING` copied as `licenses/in-tree/contrib__unifdef__COPYING` |
| `contrib/unvis/` | 3 | 0 | no SPDX tags | none found by name |
| `contrib/vis/` | 5 | 0 | no SPDX tags | none found by name |
| `contrib/wireguard-tools/` | 28 | 25 | `GPL-2.0 OR MIT` (22); `MIT` (3) | none found by name |
| `contrib/wpa/` | 879 | 0 | no SPDX tags | `contrib/wpa/COPYING` copied as `licenses/in-tree/contrib__wpa__COPYING` |
| `contrib/xz/` | 220 | 211 | `0BSD` (211) | `contrib/xz/COPYING` copied as `licenses/in-tree/contrib__xz__COPYING`<br>`contrib/xz/COPYING.0BSD` copied as `licenses/in-tree/contrib__xz__COPYING.0BSD` |
| `sys/contrib/alpine-hal/` | 52 | 0 | no SPDX tags | none found by name |
| `sys/contrib/ck/` | 81 | 0 | no SPDX tags | none found by name |
| `sys/contrib/dev/` | 1581 | 986 | `GPL-2.0 OR BSD-3-Clause` (429); `BSD-3-Clause-Clear` (388); `ISC` (162) | `sys/contrib/dev/ice/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__ice__LICENSE`<br>`sys/contrib/dev/imx/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__imx__LICENSE`<br>`sys/contrib/dev/ipw/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__ipw__LICENSE`<br>`sys/contrib/dev/iwi/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__iwi__LICENSE`<br>`sys/contrib/dev/iwm/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__iwm__LICENSE`<br>`sys/contrib/dev/iwn/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__iwn__LICENSE`<br>`sys/contrib/dev/mwl/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__mwl__LICENSE`<br>`sys/contrib/dev/nvidia/LICENCE.nvidia` copied as `licenses/in-tree/sys__contrib__dev__nvidia__LICENCE.nvidia`<br>`sys/contrib/dev/qat/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__qat__LICENSE`<br>`sys/contrib/dev/qat_c2xxx/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__qat_c2xxx__LICENSE`<br>`sys/contrib/dev/ral/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__ral__LICENSE`<br>`sys/contrib/dev/rtwn/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__rtwn__LICENSE`<br>`sys/contrib/dev/run/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__run__LICENSE`<br>`sys/contrib/dev/wpi/LICENSE` copied as `licenses/in-tree/sys__contrib__dev__wpi__LICENSE` |
| `sys/contrib/device-tree/` | 16901 | 13156 | `(GPL-2.0-only OR BSD-2-Clause)` (3620); `GPL-2.0` (2132); `(GPL-2.0+ OR MIT)` (1377) | `sys/contrib/device-tree/COPYING` copied as `licenses/in-tree/sys__contrib__device-tree__COPYING` |
| `sys/contrib/dpdk_rte_lpm/` | 14 | 13 | `BSD-3-Clause` (10); `BSD-2-Clause` (3) | none found by name |
| `sys/contrib/edk2/` | 162 | 160 | `BSD-2-Clause-Patent` (160) | none found by name |
| `sys/contrib/ena-com/` | 10 | 9 | `BSD-3-Clause` (9) | none found by name |
| `sys/contrib/libb2/` | 17 | 0 | no SPDX tags | none found by name |
| `sys/contrib/libfdt/` | 13 | 0 | no SPDX tags | none found by name |
| `sys/contrib/libnv/` | 7 | 7 | `BSD-2-Clause` (7) | none found by name |
| `sys/contrib/libsodium/` | 534 | 0 | no SPDX tags | `sys/contrib/libsodium/LICENSE` copied as `licenses/in-tree/sys__contrib__libsodium__LICENSE` |
| `sys/contrib/ncsw/` | 151 | 0 | no SPDX tags | none found by name |
| `sys/contrib/openzfs/` | 4452 | 3921 | `CDDL-1.0` (3544); `BSD-2-Clause` (100); `GPL-2.0-or-later` (70) | `sys/contrib/openzfs/COPYRIGHT` copied as `licenses/in-tree/sys__contrib__openzfs__COPYRIGHT`<br>`sys/contrib/openzfs/LICENSE` copied as `licenses/in-tree/sys__contrib__openzfs__LICENSE`<br>`sys/contrib/openzfs/NOTICE` copied as `licenses/in-tree/sys__contrib__openzfs__NOTICE`<br>`sys/contrib/openzfs/contrib/debian/copyright` copied as `licenses/in-tree/sys__contrib__openzfs__contrib__debian__copyright`<br>`sys/contrib/openzfs/contrib/icp/gcm-simd/boringssl/LICENSE` copied as `licenses/in-tree/sys__contrib__openzfs__contrib__icp__gcm-simd__boringssl__LICENSE`<br>`sys/contrib/openzfs/contrib/intel_qat/patch/LICENSE` copied as `licenses/in-tree/sys__contrib__openzfs__contrib__intel_qat__patch__LICENSE`<br>`sys/contrib/openzfs/contrib/pyzfs/LICENSE` copied as `licenses/in-tree/sys__contrib__openzfs__contrib__pyzfs__LICENSE` |
| `sys/contrib/pcg-c/` | 1 | 1 | `(Apache-2.0 OR MIT)` (1) | none found by name |
| `sys/contrib/rdma/` | 5 | 0 | no SPDX tags | none found by name |
| `sys/contrib/v4l/` | 2 | 0 | no SPDX tags | none found by name |
| `sys/contrib/vchiq/` | 34 | 0 | no SPDX tags | none found by name |
| `sys/contrib/x86emu/` | 3 | 0 | no SPDX tags | none found by name |
| `sys/contrib/xen/` | 74 | 0 | no SPDX tags | `sys/contrib/xen/COPYING` copied as `licenses/in-tree/sys__contrib__xen__COPYING` |
| `sys/contrib/xz-embedded/` | 27 | 0 | no SPDX tags | `sys/contrib/xz-embedded/COPYING` copied as `licenses/in-tree/sys__contrib__xz-embedded__COPYING` |
| `sys/contrib/zlib/` | 43 | 0 | no SPDX tags | `sys/contrib/zlib/LICENSE` copied as `licenses/in-tree/sys__contrib__zlib__LICENSE` |
| `sys/contrib/zstd/` | 154 | 0 | no SPDX tags | `sys/contrib/zstd/COPYING` copied as `licenses/in-tree/sys__contrib__zstd__COPYING`<br>`sys/contrib/zstd/LICENSE` copied as `licenses/in-tree/sys__contrib__zstd__LICENSE` |

### License files found elsewhere in the tree

| Path | Copy |
|---|---|
| `crypto/heimdal/LICENSE` | `licenses/in-tree/crypto__heimdal__LICENSE` |
| `crypto/krb5/NOTICE` | `licenses/in-tree/crypto__krb5__NOTICE` |
| `crypto/krb5/src/lib/gssapi/LICENSE` | `licenses/in-tree/crypto__krb5__src__lib__gssapi__LICENSE` |
| `crypto/libecc/LICENSE` | `licenses/in-tree/crypto__libecc__LICENSE` |
| `crypto/openssh/LICENCE` | `licenses/in-tree/crypto__openssh__LICENCE` |
| `crypto/openssl/LICENSE.txt` | `licenses/in-tree/crypto__openssl__LICENSE.txt` |
| `crypto/openssl/external/perl/Text-Template-1.56/LICENSE` | `licenses/in-tree/crypto__openssl__external__perl__Text-Template-1.56__LICENSE` |
| `lib/libc/regex/COPYRIGHT` | `licenses/in-tree/lib__libc__regex__COPYRIGHT` |
| `sys/dev/e1000/LICENSE` | `licenses/in-tree/sys__dev__e1000__LICENSE` |
| `sys/dev/vmware/pvscsi/LICENSE` | `licenses/in-tree/sys__dev__vmware__pvscsi__LICENSE` |
| `sys/dev/vmware/vmci/LICENSE` | `licenses/in-tree/sys__dev__vmware__pvscsi__LICENSE` |
| `tests/sys/capsicum/LICENSE` | `licenses/in-tree/tests__sys__capsicum__LICENSE` |
| `tests/sys/mac/portacl/LICENSE` | `licenses/in-tree/tests__sys__mac__portacl__LICENSE` |
| `tools/regression/zfs/LICENSE` | `licenses/in-tree/tools__regression__zfs__LICENSE` |
| `usr.sbin/crunch/COPYRIGHT` | `licenses/in-tree/usr.sbin__crunch__COPYRIGHT` |

The paths of every in-tree license file are in the two tables above. Files with identical contents are copied once, under the first path; the other paths with the same contents are listed in `licenses/in-tree/INDEX.txt`.

Files whose names match the license-file patterns but are code, tests or documents that mention a license (not copied): `contrib/ntp/html/copyright.html`, `contrib/ntp/sntp/include/copyright.def`, `contrib/pkgconf/libpkgconf/license.c`, `contrib/pkgconf/t/sbom/license-file-bar.test`, `contrib/pkgconf/t/sbom/license-file-foo.test`, `contrib/pkgconf/t/sbom/license-isc.test`, `contrib/pkgconf/t/sbom/license-malformed-token.test`, `contrib/pkgconf/t/sbom/license-noassertion.test`, `contrib/pkgconf/tests/lib1/license-malformed.pc`, `crypto/heimdal/doc/copyright.texi`, `crypto/krb5/src/include/copyright.h`, `crypto/krb5/src/util/ss/copyright.h`, `crypto/krb5/src/windows/installer/wix/lang/license.rtf`, `crypto/openssl/util/perl/OpenSSL/copyright.pm`, `sys/sys/copyright.h`.

## SPDX identifiers found

The ids below occur in tags. Each text is the SPDX license-list-data text (see "Texts and their sources"), with its template placeholders as SPDX publishes them. A tag only says what the file's author declared; it does not give the notices that accompany it, which stay in the files themselves.

| Tag token | Files with it | Text |
|---|---|---|
| `Apache-2.0` | 15660 | `licenses/spdx/Apache-2.0.txt` |
| `LLVM-exception` | 15629 | `licenses/spdx/LLVM-exception.txt` |
| `BSD-2-Clause` | 15199 | `licenses/spdx/BSD-2-Clause.txt` |
| `GPL-2.0-only` | 5815 | `licenses/spdx/GPL-2.0-only.txt` |
| `GPL-2.0` | 4692 | `licenses/spdx/GPL-2.0.txt` |
| `BSD-3-Clause` | 4240 | `licenses/spdx/BSD-3-Clause.txt` |
| `MIT` | 3706 | `licenses/spdx/MIT.txt` |
| `CDDL-1.0` | 3545 | `licenses/spdx/CDDL-1.0.txt` |
| `GPL-2.0+` | 2193 | `licenses/spdx/GPL-2.0+.txt` |
| `GPL-2.0-or-later` | 1058 | `licenses/spdx/GPL-2.0-or-later.txt` |
| `ISC` | 566 | `licenses/spdx/ISC.txt` |
| `BSD-4-Clause` | 444 | `licenses/spdx/BSD-4-Clause.txt` |
| `BSD-3-Clause-Clear` | 391 | `licenses/spdx/BSD-3-Clause-Clear.txt` |
| `0BSD` | 217 | `licenses/spdx/0BSD.txt` |
| `BSD-2-Clause-Patent` | 166 | `licenses/spdx/BSD-2-Clause-Patent.txt` |
| `X11` | 62 | `licenses/spdx/X11.txt` |
| `Linux-OpenIB` | 61 | `licenses/spdx/Linux-OpenIB.txt` |
| `pkgconf` | 59 | `licenses/spdx/pkgconf.txt` |
| `BSD-2-Clause-FreeBSD` | 58 | `licenses/spdx/BSD-2-Clause-FreeBSD.txt` |
| `MIT-CMU` | 46 | `licenses/spdx/MIT-CMU.txt` |
| `CC0-1.0` | 40 | `licenses/spdx/CC0-1.0.txt` |
| `FSFAP` | 29 | `licenses/spdx/FSFAP.txt` |
| `Beerware` | 22 | `licenses/spdx/Beerware.txt` |
| `FSFULLR` | 19 | `licenses/spdx/FSFULLR.txt` |
| `BSD-1-Clause` | 18 | `licenses/spdx/BSD-1-Clause.txt` |
| `Linux-syscall-note` | 8 | `licenses/spdx/Linux-syscall-note.txt` |
| `RSA-MD` | 7 | `licenses/spdx/RSA-MD.txt` |
| `BSD-4.3TAHOE` | 4 | `licenses/spdx/BSD-4.3TAHOE.txt` |
| `Brian-Gladman-3-Clause` | 4 | `licenses/spdx/Brian-Gladman-3-Clause.txt` |
| `GPL-3.0-or-later` | 2 | `licenses/spdx/GPL-3.0-or-later.txt` |
| `Autoconf-exception-macro` | 2 | `licenses/spdx/Autoconf-exception-macro.txt` |
| `Unlicense` | 1 | `licenses/spdx/Unlicense.txt` |
| `LGPL-2.1-or-later` | 1 | `licenses/spdx/LGPL-2.1-or-later.txt` |
| `Autoconf-exception-generic` | 1 | `licenses/spdx/Autoconf-exception-generic.txt` |
| `OpenSSL-standalone` | 1 | `licenses/spdx/OpenSSL-standalone.txt` |
| `MPL-2.0` | 1 | `licenses/spdx/MPL-2.0.txt` |
| `BSD-2-Clause-NetBSD` | 1 | `licenses/spdx/BSD-2-Clause-NetBSD.txt` |
| `Unicode-3.0` | 1 | `licenses/spdx/Unicode-3.0.txt` |
| `Caldera-no-preamble` | 1 | `licenses/spdx/Caldera-no-preamble.txt` |

Tag spellings that are not on the SPDX list and for which no text was copied: `BSD-3-clause`, `GPL-1+`, `CDDL`, `FreeBSD-2-Clause`, `LicenseRef-scancode-sudo`, `LicenseRef-OpenZFS-ThirdParty-PublicDomain`. `LicenseRef-OpenZFS-ThirdParty-PublicDomain` and `LicenseRef-scancode-sudo` are referenced from the files that use them; find them with `git grep`. `GPL-1+` and `CDDL` are older spellings of GPL-1.0-or-later and CDDL-1.0.

## How far the scan goes

- Of 114872 tracked files, 48107 carry an SPDX tag in the first 8 KiB. 31447 more have no tag but contain copyright or license wording in the first 8 KiB (a header, not classified here). 35180 have neither (binary files, data, tests, generated files, short files), and their license is not determined by this scan.
- The scan reads only the first 8 KiB of each file, matches only the `SPDX-License-Identifier:` tag, and finds license files only by name. License text that lives in a file under another name, in a README, or deeper than 8 KiB is not found.
- Several in-tree licenses are not BSD-style: for example the firmware and driver licenses under `sys/contrib/dev/` (including `sys/contrib/dev/nvidia/LICENCE.nvidia`), `sys/contrib/openzfs`, `contrib/llvm-project`, `crypto/openssl`, and the GPL-licensed components under `contrib/`. Read those files before redistributing from this tree.

## Texts and their sources

- `BSD-2-Clause.txt`: the SPDX BSD-2-Clause text from spdx/license-list-data (commit 31846481de7ea17d92f2f46fad76abe58b2e9bb0, `text/BSD-2-Clause.txt`), with the template line `Copyright (c) <year> <owner>` removed on the operator's instruction. The rest is unchanged.
- `FreeBSD-COPYRIGHT.txt`: byte for byte the root `COPYRIGHT` of this repository.
- `spdx/*.txt`: spdx/license-list-data at commit 31846481de7ea17d92f2f46fad76abe58b2e9bb0, files under `text/`, unmodified. For ids that SPDX lists as deprecated (`GPL-2.0`, `GPL-2.0+`, `BSD-2-Clause-FreeBSD`, `BSD-2-Clause-NetBSD`) the text is the file `text/deprecated_<id>.txt`. `spdx/BSD-2-Clause.txt` is the unmodified template, unlike `BSD-2-Clause.txt` one level up.
- `in-tree/*`: the files named in the tables, copied unchanged from this tree; the file name is the original path with `/` replaced by `__`.
