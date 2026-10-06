#!/bin/sh
set -eu
ROOT=$(pwd)
BIN="$ROOT/plycount"
SCRATCH=${TMPDIR:-/tmp}
work=$(mktemp -d "$SCRATCH/plycount-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

"$BIN" --help "$work/does-not-exist.pgn" >"$work/help1" 2>"$work/err"
"$BIN" -h >"$work/help2"
"$BIN" -? >"$work/help3"
cmp "$work/help1" "$work/help2"
cmp "$work/help1" "$work/help3"
grep -q 'RAV moves are not counted' "$work/help1"
"$BIN" --version | grep -q '^plycount '

if "$BIN" --wat >"$work/out" 2>"$work/err"; then exit 1; fi
grep -q 'unknown option' "$work/err"
if "$BIN" >"$work/out" 2>"$work/err"; then exit 1; fi
if "$BIN" missing.pgn >"$work/out" 2>"$work/err"; then exit 1; fi
if "$BIN" --verify "$work/no-input.pgn" out.pgn >"$work/out" 2>"$work/err"; then exit 1; fi
if "$BIN" --limit 0 missing.pgn out.pgn >"$work/out" 2>"$work/err"; then exit 1; fi
if "$BIN" --limit -1 missing.pgn out.pgn >"$work/out" 2>"$work/err"; then exit 1; fi
if "$BIN" --limit 18446744073709551616 missing.pgn out.pgn >"$work/out" 2>"$work/err"; then exit 1; fi

cat >"$work/in.pgn" <<'PGN'
[Event "count fixture"]
[Result "1-0"]
[PlyCount "999"]

1. e4 $1 {ignore 18. Qh5} (1. d4 d5 (1... Nf6)) e5 2. Nf3 $2 Nc6 1-0
[Event "second"]
[SetUp "1"]
[FEN "8/8/8/8/8/8/8/K6k b - - 0 37"]
[Result "*"]

*
PGN
"$BIN" --limit 1 "$work/in.pgn" "$work/out.pgn"
grep -q '\[PlyCount "4"\]' "$work/out.pgn"
"$BIN" --verify --limit 1 "$work/out.pgn"
if "$BIN" --verify "$work/out.pgn" extra >"$work/out" 2>"$work/err"; then exit 1; fi
if "$BIN" "$work/in.pgn" "$work/in.pgn" >"$work/out" 2>"$work/err"; then exit 1; fi
printf 'sentinel output\n' >"$work/limited.pgn"
if "$BIN" "$work/in.pgn" "$work/limited.pgn" >"$work/out" 2>"$work/err"; then exit 1; fi
test "$(cat "$work/limited.pgn")" = 'sentinel output'
"$BIN" --force --limit 1 "$work/in.pgn" "$work/limited.pgn"
"$BIN" --verify --limit 1 "$work/limited.pgn"
: >"$work/empty.pgn"
mkdir -p "$work/failed output"
if "$BIN" "$work/empty.pgn" "$work/failed output/empty out.pgn" >"$work/out" 2>"$work/err"; then exit 1; fi
test ! -e "$work/failed output/empty out.pgn"
test -z "$(find "$work/failed output" -maxdepth 1 -name '.plycount.*' -print)"
"$BIN" "$work/in.pgn" "$work/all.pgn"
test "$(grep -c '^\[PlyCount ' "$work/all.pgn")" -eq 2
grep -q '^\[PlyCount "0"\]' "$work/all.pgn"
"$BIN" --verify "$work/all.pgn"
mkdir -p "$work/current directory" "$work/other directory"
cp "$work/in.pgn" "$work/current directory/input.pgn"
(cd "$work/current directory" && "$BIN" input.pgn 'output file.pgn')
"$BIN" --verify "$work/current directory/output file.pgn"
test -z "$(find "$work/current directory" -maxdepth 1 -name '.plycount.*' -print)"
"$BIN" "$work/in.pgn" "$work/other directory/output with spaces.pgn"
"$BIN" --verify "$work/other directory/output with spaces.pgn"
test -z "$(find "$work/other directory" -maxdepth 1 -name '.plycount.*' -print)"

cat >"$work/verify-mix.pgn" <<'PGN'
[Event "mismatch first"]
[PlyCount "0"]
[Result "*"]

1. e4 *
[Event "correct second"]
[PlyCount "2"]
[Result "*"]

1. e4 e5 *
[Event "invalid"]
[PlyCount "abc"]
[Result "*"]

1. Nf3 *
[Event "duplicate"]
[PlyCount "1"]
[PlyCount "1"]
[Result "*"]

1. d4 *
[Event "missing"]
[Result "*"]

1. c4 *
[Event "mismatch last"]
[PlyCount "42"]
[Result "*"]

1. Nf3 *
PGN
if "$BIN" --verify "$work/verify-mix.pgn" >"$work/verify-out" 2>"$work/verify-err"; then exit 1; else test "$?" -eq 1; fi
grep -q 'game 1, line 2: PlyCount 0, calculated 1' "$work/verify-err"
if grep -q '^plycount: game 2:' "$work/verify-err"; then exit 1; fi
grep -q 'game 3, line 12: invalid PlyCount "abc"' "$work/verify-err"
grep -q 'game 4: duplicate PlyCount tags (2 total): line 17 "1", line 18 "1"' \
    "$work/verify-err"
grep -q 'game 5, starting at line 22: missing PlyCount' "$work/verify-err"
grep -q 'game 6, line 27: PlyCount 42, calculated 1' "$work/verify-err"
grep -q 'checked 6 games: 1 correct, 1 missing, 2 mismatched, 1 invalid, 1 duplicate' "$work/verify-err"
if "$BIN" --verify --limit 2 "$work/verify-mix.pgn" >"$work/verify-out" 2>"$work/verify-err"; then exit 1; else test "$?" -eq 1; fi
grep -q 'checked 2 games: 1 correct, 0 missing, 1 mismatched' "$work/verify-err"
grep -q 'game 1, line 2: PlyCount 0, calculated 1' "$work/verify-err"
cat >"$work/verify-second-bad.pgn" <<'PGN'
[Event "correct first"]
[PlyCount "1"]
[Result "*"]

1. e4 *
[Event "bad second"]
[PlyCount "7"]
[Result "*"]

1. d4 *
PGN
if "$BIN" --verify "$work/verify-second-bad.pgn" >"$work/verify-out" 2>"$work/verify-err"; then exit 1; else test "$?" -eq 1; fi
grep -q 'game 2, line 7: PlyCount 7, calculated 1' "$work/verify-err"
grep -q 'checked 2 games: 1 correct, 0 missing, 1 mismatched' "$work/verify-err"

awk '{ printf "%s\r\n", $0 }' "$work/verify-mix.pgn" \
    >"$work/verify-crlf.pgn"
if "$BIN" --verify "$work/verify-crlf.pgn" >"$work/verify-out" 2>"$work/verify-err"; then exit 1; else test "$?" -eq 1; fi
grep -q 'game 1, line 2: PlyCount 0, calculated 1' "$work/verify-err"
grep -q 'game 4: duplicate PlyCount tags (2 total): line 17 "1", line 18 "1"' \
    "$work/verify-err"
grep -q 'game 6, line 27: PlyCount 42, calculated 1' "$work/verify-err"

cat >"$work/verify-three-duplicates.pgn" <<'PGN'
[Event "three duplicate values"]
[PlyCount "1"]
[PlyCount "1"]
[PlyCount "9"]
[Result "*"]

1. e4 *
PGN
if "$BIN" --verify "$work/verify-three-duplicates.pgn" >"$work/verify-out" 2>"$work/verify-err"; then exit 1; else test "$?" -eq 1; fi
grep -q 'game 1: duplicate PlyCount tags (3 total): line 2 "1", line 3 "1", line 4 "9"' \
    "$work/verify-err"
grep -q 'checked 1 games: 0 correct, 0 missing, 0 mismatched, 0 invalid, 1 duplicate' \
    "$work/verify-err"

: >"$work/many-missing.pgn"
i=0
while [ "$i" -lt 105 ]; do
    cat >>"$work/many-missing.pgn" <<'PGN'
[Event "missing tag"]
[Result "*"]

*
PGN
    i=$((i + 1))
done
if "$BIN" --verify "$work/many-missing.pgn" >"$work/verify-out" 2>"$work/verify-err"; then exit 1; else test "$?" -eq 1; fi
test "$(grep -c '^plycount: game ' "$work/verify-err")" -eq 100
grep -q 'further discrepancy details suppressed after 100 reports' "$work/verify-err"
grep -q 'checked 105 games: 0 correct, 105 missing' "$work/verify-err"
echo 'CLI/regression tests passed'
