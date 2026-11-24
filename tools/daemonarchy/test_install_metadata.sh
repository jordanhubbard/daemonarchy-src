#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Exercise native install recipes; this does not compile the fixture payloads.
set -eu

if [ "$#" -lt 4 ] || [ "$#" -gt 5 ]; then
	echo "usage: $0 source-root make-executable install-executable" \
	    "work-dir [mk-overlay]" >&2
	exit 2
fi
source_root=$1
make_bin=$2
install_bin=$3
work_dir=$4
mk_overlay=${5-}
test -d "$source_root/share/mk"
test -x "$make_bin"
test -x "$install_bin"
test -c /dev/filemon
case "$work_dir" in
/*) ;;
*) echo "work-dir must be absolute" >&2; exit 2 ;;
esac
# Retain both successful and failed workspaces for independent inspection.
mkdir "$work_dir"
cd "$work_dir"
mkdir -p dest/bin dest/usr/lib/debug/bin

cat > Makefile <<'EOF'
PROGS= first_program second_program
BINDIR= /bin
MAN=
.include <bsd.progs.mk>
EOF
for program in first_program second_program; do
	printf '#!/bin/sh\nprintf "%s\\n"\n' "$program" > "$program"
	chmod 755 "$program"
	printf 'debug payload for %s\n' "$program" > "$program.debug"
done

set -- "$make_bin"
if [ -n "$mk_overlay" ]; then
	set -- "$@" -m "$mk_overlay"
fi
set -- "$@" -m "$source_root/share/mk"
MAKEOBJDIR="$work_dir" "$@" -j2 "SRCTOP=$source_root" "OBJTOP=$work_dir" \
    __MAKE_CONF=/dev/null SRC_ENV_CONF=/dev/null SRCCONF=/dev/null \
    MK_MAN=no MK_DEBUG_FILES=yes STRIP= NO_ROOT=yes DB_FROM_SRC=yes \
    "INSTALL=$install_bin -U" "DESTDIR=$work_dir/dest" \
    '.MAKE.MODE=meta curdirok=yes missing-meta=yes missing-filemon=yes' \
    install > make.stdout 2> make.stderr

for program in first_program second_program; do
	cmp "$program" "dest/bin/$program"
	cmp "$program.debug" "dest/usr/lib/debug/bin/$program.debug"
	for metadata in "$program._proginstall.meta" \
	    "$program.debug._debuginstall.meta"; do
		if [ ! -f "$metadata" ]; then
			echo "missing per-payload metadata: $metadata" >&2
			exit 1
		fi
		awk '
		/^-- filemon acquired metadata --$/ {
			blocks++; active = 1; next
		}
		active {
			if (bye) bad = 1
			if (stop && $0 != "# Bye bye") bad = 1
			if ($0 ~ /^# Start /) starts++
			if ($0 ~ /^V /) {
				versions++
				if ($0 != "V 5" || starts != 1) bad = 1
			}
			if ($0 ~ /^# Stop /) { stops++; stop = 1 }
			if ($0 == "# Bye bye") { byes++; bye = 1 }
		}
		END {
			exit !(blocks == 1 && starts == 1 && versions == 1 &&
			    stops == 1 && byes == 1 && !bad)
		}' "$metadata"
	done
done
test ! -e _proginstall.meta
test ! -e _debuginstall.meta
echo "PASS: four distinct install traces; four payloads preserved"
