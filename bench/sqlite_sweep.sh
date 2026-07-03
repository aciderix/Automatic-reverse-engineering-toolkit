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
rc=0
if diff -q "$OUT/aret.txt" "$OUT/wine.txt" >/dev/null; then
  echo "sqlite feature sweep (:memory:): ALL bit-identical to Wine ($total labelled features + DDL/DML)"
else
  echo "sqlite feature sweep (:memory:): DIVERGENCE (ARET '<' vs Wine '>')"
  diff "$OUT/aret.txt" "$OUT/wine.txt" | grep -E "^[<>]" | head -40
  rc=1
fi

# On-disk pass: the same feature battery against a real FILE database, plus the
# file-touching shell paths — persistence across processes, ATTACH, .backup, and
# .read of a SQL script. These exercise the OS/CRT file layer (CreateFile, the
# stat family, …) that :memory: never touches, so a wrong file/stat shim shows up
# here as a divergence against the same PE under Wine, not a silent pass.
run_disk() { # $1=tag  $2=engine runner (function)  $3=output transcript
  local tag="$1" run="$2" outf="$3"
  local db="$OUT/disk_$tag.db" db2="$OUT/disk2_$tag.db" bak="$OUT/disk_$tag.bak"
  local script="$OUT/script_$tag.sql"
  rm -f "$db" "$db2" "$bak"
  printf "CREATE TABLE imp(x);\nINSERT INTO imp VALUES(42),(43);\n" > "$script"
  {
    $run "$db" < "$SQL"                                              # feature battery, on disk
    printf "CREATE TABLE p(a,b);\nINSERT INTO p VALUES(1,'x'),(2,'y'),(3,'z');\n" | $run "$db2" >/dev/null
    printf "SELECT 'persist',count(*),sum(a) FROM p;\n" | $run "$db2"   # reopened in a fresh process
    printf "ATTACH '%s' AS d;\nSELECT 'attach',(SELECT count(*) FROM p);\nDETACH d;\n.backup %s\n" "$db2" "$bak" | $run "$db" >/dev/null
    printf "SELECT 'backup',sum(a) FROM p;\n" | $run "$bak"          # .backup produced a valid DB
    printf ".read %s\nSELECT 'read',sum(x) FROM imp;\n" "$script" | $run "$db"  # .read a SQL file (stat family)
  } 2>/dev/null | sed 's/\r$//' > "$outf"
}
aret_run() { "$OUT/t/app" "$@"; }
wine_run() { wine "$CACHE" "$@"; }
run_disk aret aret_run "$OUT/disk_aret.txt"
run_disk wine wine_run "$OUT/disk_wine.txt"
if diff -q "$OUT/disk_aret.txt" "$OUT/disk_wine.txt" >/dev/null; then
  echo "sqlite feature sweep (on-disk + persist/ATTACH/.backup/.read): bit-identical to Wine"
else
  echo "sqlite feature sweep (on-disk): DIVERGENCE (ARET '<' vs Wine '>')"
  diff "$OUT/disk_aret.txt" "$OUT/disk_wine.txt" | grep -E "^[<>]" | head -40
  rc=1
fi
exit $rc
