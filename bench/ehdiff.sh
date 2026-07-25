#!/usr/bin/env bash
# MSVC exception-handling differential: for each fixture in bench/eh/, build the clang
# MSVC-ABI PE (bench/eh/build.sh), run it under Wine (ground truth) and under ARET
# (transpile + run), and compare stdout. This is the oracle for the SEH/C++ EH bricks
# (_except_handler3 / __CxxFrameHandler / _CxxThrowException) — the winecorpus harness
# can't cover them because mingw does not emit the MSVC EH model.
#
# Skips (does not fail) when clang/lld-link/wine are unavailable, like the other harnesses.
set -u
ARET="${ARET:-target/release/aret}"
ARET="$(readlink -f "$ARET" 2>/dev/null || echo "$ARET")"
DIR="$(cd "$(dirname "$0")" && pwd)"
EH="$DIR/eh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export WINEDEBUG="${WINEDEBUG:--all}" WINEPREFIX="$TMP/wp" TZ=UTC LC_ALL=C
for t in clang lld-link llvm-dlltool wine; do
  command -v "$t" >/dev/null 2>&1 || { echo "SKIP  ($t unavailable)"; exit 0; }
done
[ -f /usr/i686-w64-mingw32/lib/libmsvcrt.a ] || { echo "SKIP  (mingw i686 import libs unavailable)"; exit 0; }
wine wineboot --init >/dev/null 2>&1 || true
extract() { awk '/--- program output ---/{f=1;next} f{sub(/^  \| ?/,"");print}'; }

pass=0; total=0
for src in "$EH"/*.c "$EH"/*.cpp; do
  [ -e "$src" ] || continue
  case "$src" in */eh_support.c) continue;; esac   # linked into every fixture, not a test
  name="$(basename "$src")"; name="${name%.*}"; total=$((total+1))
  if ! bash "$EH/build.sh" "$src" "$TMP/$name.exe" 2>"$TMP/err"; then
    echo "FAIL  $name (build: $(head -1 "$TMP/err"))"; continue
  fi
  wine "$TMP/$name.exe" >"$TMP/wine.out" 2>/dev/null
  "$ARET" "$TMP/$name.exe" --mode transpile --out-dir "$TMP/$name.out" --run 2>/dev/null \
    | extract >"$TMP/aret.out"
  if diff -q <(tr -d '\r' <"$TMP/wine.out") <(tr -d '\r' <"$TMP/aret.out") >/dev/null; then
    echo "  ok    $name"; pass=$((pass+1))
  else
    echo "DIFF  $name"; diff <(tr -d '\r' <"$TMP/wine.out") <(tr -d '\r' <"$TMP/aret.out") | head -8
  fi
done
echo "------------------------------------------"
echo "MSVC EH differential: $pass/$total fixtures"
[ "$pass" = "$total" ]
