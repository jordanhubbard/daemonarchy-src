#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Native production-owner checks, not a firmware or release receipt.
set -eu
if [ "$#" -ne 2 ] || [ "$(uname -s)" != FreeBSD ]; then
    echo "usage (native FreeBSD): sh $0 SOURCE_ROOT EXISTING_EXTERNAL_OBJECT_DIR" >&2
    exit 2
fi
source_root=$(realpath "$1")
object_dir=$(realpath "$2")
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
case "$object_dir/" in
    "$source_root/"*) echo "objects must be outside source" >&2; exit 2 ;;
esac
binary=$object_dir/netdev-ownership-test
if [ -e "$binary" ] || [ -L "$binary" ]; then
    echo "preserve existing test output: $binary" >&2
    exit 2
fi
"${CC:-/usr/bin/cc}" -std=gnu17 -Wall -Wextra -Werror \
    -Wno-unused-parameter -fno-builtin -DUSER_MALLOC -D_STANDALONE \
    -g -fno-omit-frame-pointer -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$source_root" -I "$source_root/stand/libsa" -I "$source_root/stand/common" \
    "$test_dir/netdev-ownership.c" -o "$binary"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$binary"
