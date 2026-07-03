#!/usr/bin/env bash
# Systematic feature differential for a REAL target binary: transpile the actual
# Sysinternals-grade sqlite3.exe (MSVC, stripped) with ARET and run a broad,
# DETERMINISTIC SQL feature battery (bench/sqlite_sweep.sql) against the SAME
# binary under Wine (ground truth). Any line that differs is a real gap or bug.
#
# Complements winediff (small synthetic programs): this measures how much of a
# large real binary's *feature surface* ARET reproduces bit-for-bit — the sweep
# that, missing, once let a broken transcendental ship as "complete".
#
# Skips (does not fail) when wine, the network, or the aret binary are missing.
set -u
ARET="${ARET:-target/release/aret}"
ARET="$(readlink -f "$ARET" 2>/dev/null || echo "$ARET")"
DIR="$(cd "$(dirname "$0")" && pwd)"
SQL="$DIR/sqlite_sweep.sql"
VER=3400100  # sqlite 3.40.1 (2022) — MSVC 32-bit stripped, versioned & public
URL="https://www.sqlite.org/2022/sqlite-tools-win32-x86-${VER}.zip"
CACHE="${SQLITE_EXE:-$DIR/.cache/sqlite3-${VER}.exe}"

command -v wine >/dev/null 2>&1 || { echo "SKIP (wine unavailable)"; exit 0; }
[ -x "$ARET" ] || { echo "SKIP (aret binary not built: $ARET)"; exit 0; }

if [ ! -f "$CACHE" ]; then
  mkdir -p "$(dirname "$CACHE")"
  TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
  if ! curl -fsS --max-time 60 -o "$TMP/s.zip" -L "$URL" 2>/dev/null; then
    echo "SKIP (cannot download sqlite3.exe)"; exit 0; fi
  python3 -c "import zipfile,sys;zipfile.ZipFile('$TMP/s.zip').extract('sqlite-tools-win32-x86-${VER}/sqlite3.exe','$TMP')" 2>/dev/null \
    || { echo "SKIP (unzip failed)"; exit 0; }
  cp "$TMP/sqlite-tools-win32-x86-${VER}/sqlite3.exe" "$CACHE"
fi

OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
if ! "$ARET" --mode transpile --out-dir "$OUT/t" "$CACHE" >/dev/null 2>&1; then
  echo "FAIL (transpile of sqlite3.exe failed)"; exit 1; fi

export WINEDEBUG=-all WINEPREFIX="$OUT/wp"
"$OUT/t/app" :memory: < "$SQL" 2>/dev/null > "$OUT/aret.txt"
wine "$CACHE" :memory: < "$SQL" 2>/dev/null | sed 's/\r$//' > "$OUT/wine.txt"

total=$(grep -c "^SELECT '" "$SQL")
if diff -q "$OUT/aret.txt" "$OUT/wine.txt" >/dev/null; then
  echo "sqlite feature sweep: ALL bit-identical to Wine ($total labelled features + DDL/DML)"
  exit 0
else
  echo "sqlite feature sweep: DIVERGENCE (ARET '<' vs Wine '>')"
  diff "$OUT/aret.txt" "$OUT/wine.txt" | grep -E "^[<>]" | head -40
  exit 1
fi
