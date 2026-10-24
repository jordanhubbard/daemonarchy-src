#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Native development build, not an installed kernel or release receipt.
set -eu
test "$(uname -s)" = FreeBSD
test "$(uname -m)" = amd64
out=$PWD/_build/earlynet-native
mkdir -p "$out/lua"
uname -a > "$out/worker.txt"
test -s "$out/source-commit.txt"
test -s "$out/source-tree.txt"
while read -r expected input; do
    test "$(sha256 -q "$input")" = "$expected"
done < "$out/feature-inputs.sha256"
df -k . /var/tmp
sysctl hw.physmem
test "$(df -k /var/tmp | awk 'NR==2 {print $4}')" -ge 4194304
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Istand/efi/loader \
    stand/efi/loader/netconsole_undi.c tools/daemonarchy/tests/netconsole-undi.c \
    -o /var/tmp/undi-admission
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 /var/tmp/undi-admission > "$out/admission.stdout"
cc -std=gnu17 -Wall -Wextra -Werror -Isys -Istand/efi/loader \
    tools/daemonarchy/tests/netconsole-protocol.c stand/efi/loader/netconsole_proto.c \
    -lmd -o /var/tmp/earlynet-protocol
/var/tmp/earlynet-protocol > "$out/protocol.stdout"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Istand/efi/loader \
    tools/daemonarchy/tests/netconsole-report.c -o /var/tmp/earlynet-usb-report
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 /var/tmp/earlynet-usb-report > "$out/report.stdout"
/usr/libexec/flua tools/daemonarchy/tests/loader-debug-setup.lua "$PWD" \
    > "$out/setup.stdout"
MAKEOBJDIRPREFIX=$(mktemp -d /var/tmp/earlynet-loader-objects.XXXXXXXX)
export MAKEOBJDIRPREFIX
make -C stand -j2 NO_ROOT=yes WITHOUT_LOADER_BIOS=yes LOADER_NET_SUPPORT=yes obj \
    > "$out/obj.log" 2>&1
if make -C stand -j2 NO_ROOT=yes WITHOUT_LOADER_BIOS=yes LOADER_NET_SUPPORT=yes all \
    > "$out/build.log" 2>&1; then
    loader_obj=$(make -C stand/efi/loader_lua -V .OBJDIR)
    cp "$loader_obj/loader_lua.efi" "$out/loader.efi"
    cp stand/lua/*.lua "$out/lua/"
    sha256 "$out/loader.efi" "$out"/lua/*.lua > "$out/SHA256"
    # A separate bring-up medium exposes firmware capability without
    # requiring the failing kernel to start or altering the 1.3 installer.
    mkdir -p "$out/usb-root/EFI/BOOT" "$out/usb-root/boot/lua" "$out/usb-root/DAEMDBG" \
        "$out/usb-root/boot/defaults"
    # EFI loader root discovery accepts a loader configuration without a
    # kernel. Keep this standalone medium self-contained and non-autobooting.
    printf 'autoboot_delay="NO"\n' > "$out/usb-root/boot/defaults/loader.conf"
    cp "$out/loader.efi" "$out/usb-root/EFI/BOOT/BOOTX64.EFI"
    cp "$out"/lua/*.lua "$out/usb-root/boot/lua/"
    printf 'Daemonarchy firmware inventory USB v1\n' > "$out/usb-root/DAEMDBG/PROBE.TAG"
    printf 'loader.setenv("daemonarchy.inventory.source", "%s")\n' \
        "$(cat "$out/source-commit.txt")" > "$out/usb-root/boot/lua/loader.lua"
    cat tools/daemonarchy/tests/firmware-inventory.lua >> "$out/usb-root/boot/lua/loader.lua"
    makefs -t msdos -s 16m -o fat_type=16 -o sectors_per_cluster=1 \
        -o volume_label=DAEMDBG131 \
        "$out/inventory.esp" "$out/usb-root" > "$out/makefs.stdout" 2>&1
    test "$(fstyp "$out/inventory.esp")" = msdosfs
    fsck_msdosfs -n "$out/inventory.esp" > "$out/fsck.stdout" 2>&1
    mkimg -s gpt -p efi:="$out/inventory.esp" \
        -o "$out/daemonarchy-1.3.1-firmware-inventory-amd64.img"
    sha256 "$out/daemonarchy-1.3.1-firmware-inventory-amd64.img" >> "$out/SHA256"
else
    tail -80 "$out/build.log"
    exit 1
fi
