#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Native development checks; not a canonical SDK or release receipt.
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: sh $0 SOURCE_ROOT EXISTING_EXTERNAL_OBJECT_DIR [sanitize]" >&2
    exit 2
fi
if [ "$(uname -s)" != FreeBSD ] || [ "$(uname -m)" != amd64 ]; then
    echo "these network demultiplexer checks require native FreeBSD amd64" >&2
    exit 2
fi
source_root=$(realpath "$1")
object_dir=$(realpath "$2")
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
sanitize=${3:-plain}
case "$sanitize" in
    plain|sanitize) ;;
    *) echo "unknown test mode: $sanitize" >&2; exit 2 ;;
esac
case "$object_dir/" in
    "$source_root/"*) echo "objects must be outside source" >&2; exit 2 ;;
esac
binary=$object_dir/netconsole-receive-test
if [ -e "$binary" ] || [ -L "$binary" ]; then
    echo "preserve existing test output: $binary" >&2
    exit 2
fi
set -- "${CC:-/usr/bin/cc}" -std=gnu17 -Wall -Wextra -Werror \
    -Wno-format -Wno-unused-parameter -Wno-sign-compare -fno-builtin \
    -DUSER_MALLOC -D_STANDALONE -DLOADER_NET_SUPPORT \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$source_root" -I "$source_root/sys" \
    -I "$source_root/stand/libsa" -I "$source_root/stand/common" \
    -I "$source_root/stand/efi/include" \
    -I "$source_root/stand/efi/include/amd64" \
    -I "$source_root/sys/contrib/edk2/Include"
if [ "$sanitize" = sanitize ]; then
    set -- "$@" -g -fno-omit-frame-pointer -fsanitize=address,undefined
fi
"$@" "$test_dir/netconsole-receive.c" "$source_root/stand/efi/loader/netconsole_proto.c" -lmd -o "$binary"
# FreeBSD lacks LeakSanitizer; environment storage uses native libc; no LeakSanitizer result is claimed.
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$binary"
