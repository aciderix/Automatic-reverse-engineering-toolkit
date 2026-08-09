#!/usr/bin/env bash
# GNU/Itanium C++ exception-handling differential: for each fixture in bench/gnueh/,
# build the mingw g++ PE (Itanium EH: __cxa_throw + a DWARF .eh_frame LSDA), run it under
# Wine (ground truth) and under ARET (transpile + run), and compare stdout. This is the
# oracle for the GNU C++ EH brick (analysis::gnu_eh + aret_cxa_throw), which the MSVC
# ehdiff.sh (clang) cannot cover and which the winecorpus harness captures flakily for
# this class (its pipe-in-$() capture; here stdout is captured to a FILE, like ehdiff).
#
# Skips (does not fail) when mingw g++/wine are unavailable, like the other harnesses.
set -u
ARET="${ARET:-target/release/aret}"
ARET="$(readlink -f "$ARET" 2>/dev/null || echo "$ARET")"
DIR="$(cd "$(dirname "$0")" && pwd)"
EH="$DIR/gnueh"
GXX="${MINGW:-i686-w64-mingw32-gcc}"; GXX="${GXX%-gcc}-g++"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export WINEDEBUG="${WINEDEBUG:--all}" WINEPREFIX="$TMP/wp" TZ=UTC LC_ALL=C
command -v "$GXX" >/dev/null 2>&1 || { echo "SKIP  ($GXX unavailable)"; exit 0; }
command -v wine >/dev/null 2>&1 || { echo "SKIP  (wine unavailable)"; exit 0; }
wine wineboot --init >/dev/null 2>&1 || true
extract() { awk '/--- program output ---/{f=1;next} f{sub(/^  \| ?/,"");print}'; }

pass=0; total=0
for src in "$EH"/*.cpp; do
  [ -e "$src" ] || continue
  name="$(basename "$src")"; name="${name%.*}"; total=$((total+1))
  if ! "$GXX" -O1 -w "$src" -o "$TMP/$name.exe" 2>"$TMP/err"; then
    echo "FAIL  $name (build: $(head -1 "$TMP/err"))"; continue
  fi
  # Copy the runtime DLLs the exe imports next to it (for the Wine oracle only).
  for d in libstdc++-6.dll libgcc_s_dw2-1.dll libwinpthread-1.dll; do
    p="$(find /usr/lib/gcc/i686-w64-mingw32 /usr/i686-w64-mingw32 -name "$d" 2>/dev/null | head -1)"
    [ -n "$p" ] && cp -n "$p" "$TMP/" 2>/dev/null || true
  done
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
echo "GNU/Itanium C++ EH differential: $pass/$total fixtures"
[ "$pass" = "$total" ]
