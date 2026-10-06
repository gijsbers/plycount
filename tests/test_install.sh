#!/bin/sh
set -eu
SCRATCH=${TMPDIR:-/tmp}
work=$(mktemp -d "$SCRATCH/plycount-install.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
dest="$work/root"
prefix=/usr/local
bindir="$dest$prefix/bin"
mandir="$dest$prefix/share/man/man1"

mkdir -p "$bindir" "$mandir"
printf 'keep me\n' >"$bindir/unrelated-sentinel"
MAKE=${MAKE:-make}
export DESTDIR="$dest" PREFIX="$prefix"
"$MAKE" install
test -x "$bindir/plycount"
test -f "$mandir/plycount.1"
groff -man -Tascii "$mandir/plycount.1" >"$work/man-render" 2>"$work/man-errors"
test ! -s "$work/man-errors"
grep -q 'PLYCOUNT(1)' "$work/man-render"
"$MAKE" uninstall
test ! -e "$bindir/plycount"
test ! -e "$mandir/plycount.1"
test "$(cat "$bindir/unrelated-sentinel")" = 'keep me'
echo 'install/uninstall tests passed'
