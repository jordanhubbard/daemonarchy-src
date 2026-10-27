#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Host check of the hardware-trace stick block map and raw rewrites with a
# fake EFI_BLOCK_IO; not a firmware or disk acceptance test.  Any host with
# a C compiler (FreeBSD or Linux).
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: sh $0 SOURCE_ROOT EXISTING_EXTERNAL_OBJECT_DIR" >&2
    exit 2
fi
source_root=$(realpath "$1")
object_dir=$(realpath "$2")
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
case "$object_dir/" in
    "$source_root/"*) echo "objects must be outside source" >&2; exit 2 ;;
esac
binary=$object_dir/boottrace-blockmap-test
rm -f "$binary"
"${CC:-cc}" -std=gnu17 -Wall -Wextra -Werror -Wno-unused-function \
    -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$source_root/stand/efi/loader" \
    "$test_dir/boottrace-blockmap.c" -o "$binary"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$binary"
