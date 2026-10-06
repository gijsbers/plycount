# plycount

PlyCount is a small C99 command-line utility for adding, updating, and
verifying `PlyCount` tags in Portable Game Notation (PGN) chess files and
databases. It is designed to stream very large collections of chess games
at high speed without loading the complete database into memory.

For each PGN chess game, `PlyCount` counts plies in the main line present in
that record's movetext. Comments, NAGs, move numbers, results, and moves inside
RAVs do not count. A FEN fullmove number does not affect the count.

Rewrite with `plycount [OPTIONS] INPUT OUTPUT`, or check existing tags with
`plycount --verify [OPTIONS] INPUT`. `--limit N` processes at most the first
N games. Existing output is preserved unless `--force` is supplied. Input
and output may not identify the same file. Help is available through
`--help`, `-h`, or `-?`.

Build and run the tests with `make` and `make test`. Install with
`make install` (the default prefix is `/usr/local`) and remove the installed
program and manpage with `make uninstall`. `make test-install` exercises both
targets in a temporary `DESTDIR`. After installation, the manual is available
as `man plycount`.

Processing is streaming and retains at most one game in memory. A single
game is limited to 64 MiB. The input is opened read-only. Rewrite output is
staged in a unique temporary file in the output directory and published by
rename after successful processing.

Verify mode checks every selected game even after finding discrepancies.
It reports at most 100 individual discrepancies, then suppresses further
per-game details while continuing to count and summarize all games. Exit
status is `0` when every selected tag is correct, `1` when one or more
discrepancies are found, and `2` for invocation or fatal processing errors.

PlyCount is licensed under the GNU General Public License version 3
(`GPL-3.0-only`); see `LICENSE`.
