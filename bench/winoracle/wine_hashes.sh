#!/usr/bin/env bash
# The Wine side of the real-Windows oracle: one `name status sha256` line per
# eligible winecorpus fixture, in exactly the format the Windows runner prints, so
# the two lists diff directly and the fixtures whose hashes differ ARE the finding.
#
# This is not a gate and never fails on a divergence — it produces a list. See
# bench/winoracle/README.md for how the two steps fit together.
#
# Eligibility must match the workflow's, or the diff compares different sets:
# a fixture is skipped when the comparison would measure the TOOLCHAIN rather than
# the API (GCC inline asm, a companion .rc/.def, a window-creating program, or a
# .nodisplay marker). Skips are printed, never dropped.
set -u

CORPUS="$(cd "$(dirname "$0")/../winecorpus" && pwd)"
MINGW="${MINGW:-i686-w64-mingw32-gcc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

command -v "$MINGW" >/dev/null 2>&1 || { echo "SKIP ($MINGW unavailable)"; exit 0; }
command -v wine     >/dev/null 2>&1 || { echo "SKIP (wine unavailable)"; exit 0; }

export WINEPREFIX="$TMP/prefix" WINEDEBUG=-all
wine wineboot --init >/dev/null 2>&1 || true

for src in "$CORPUS"/*.c; do
  name="$(basename "$src" .c)"
  case "$name" in *.dll) continue;; esac
  [ -f "$CORPUS/$name.dll.c" ] && { echo "$name SKIP-companion-dll"; continue; }
  [ -f "$CORPUS/$name.rc"    ] && { echo "$name SKIP-resource";      continue; }
  [ -f "$CORPUS/$name.def"   ] && { echo "$name SKIP-deffile";       continue; }
  [ -f "$CORPUS/$name.withdll" ] && { echo "$name SKIP-withdll";     continue; }
  [ -f "$CORPUS/$name.nodisplay" ] && { echo "$name SKIP-nodisplay"; continue; }
  grep -q '__asm__\|__attribute__' "$src" && { echo "$name SKIP-gcc-only"; continue; }
  grep -qE 'CreateWindow|DialogBox|CreateDialog' "$src" && { echo "$name SKIP-gui"; continue; }

  wd="$TMP/w/$name"; mkdir -p "$wd"
  if ! "$MINGW" -O1 -w "$src" -lversion -lole32 -loleaut32 -luser32 -lgdi32 \
        -lcomctl32 -lwinspool -llz32 -lshlwapi -ladvapi32 -lshell32 \
        -o "$wd/$name.exe" 2>/dev/null; then
    echo "$name BUILD-FAIL"; continue
  fi
  infile="$CORPUS/$name.in"; [ -f "$infile" ] || infile=/dev/null
  pargs=()
  if [ -f "$CORPUS/$name.args" ]; then
    while IFS= read -r line || [ -n "$line" ]; do pargs+=("$line"); done < "$CORPUS/$name.args"
  fi
  if ! (cd "$wd" && timeout 30 wine "$wd/$name.exe" "${pargs[@]}" <"$infile" \
        >"$wd/out.txt" 2>/dev/null); then
    echo "$name RUN-FAIL"; continue
  fi
  h=$(tr -d '\r' < "$wd/out.txt" | sha256sum | cut -c1-64)
  echo "$name OK $h"
done
