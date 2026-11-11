#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
set -eu
[ "$#" -eq 2 ] || { echo "usage: sh $0 SOURCE_ROOT EXISTING_EXTERNAL_OBJECT_DIR" >&2; exit 2; }
source_root=$(realpath "$1")
object_dir=$(realpath "$2")
case "$object_dir/" in
    "$source_root/"*) echo 'objects must be outside source' >&2; exit 2 ;;
esac
binary=$object_dir/boottrace-normal-test
[ ! -e "$binary" ] || { echo 'test output already exists' >&2; exit 2; }
"${CC:-cc}" -std=gnu17 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$source_root/stand/efi/loader" \
    "$source_root/stand/efi/loader/boottrace_normal.c" \
    "$source_root/tools/daemonarchy/tests/boottrace-normal.c" -o "$binary"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$binary"
fat_binary=$object_dir/boottrace-fat-test
[ ! -e "$fat_binary" ] || { echo 'FAT test output already exists' >&2; exit 2; }
"${CC:-cc}" -std=gnu17 -Wall -Wextra -Werror -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I "$source_root/stand/efi/loader" \
    "$source_root/stand/efi/loader/boottrace_fat.c" \
    "$source_root/tools/daemonarchy/tests/boottrace-fat.c" -o "$fat_binary"
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$fat_binary"
