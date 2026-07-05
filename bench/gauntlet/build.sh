#!/bin/bash
# Reproduce the mingw-cross-compiled gauntlet binaries from upstream source
# tarballs. Drop the *.tar.gz sources in $SRC (see README.md for URLs), then
# run this; produced .exe files land in $BINS.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-$HERE/src}"
BINS="${BINS:-$HERE/bins}"
mkdir -p "$BINS"
cd "$SRC" || { echo "no source dir $SRC (see README.md for tarball URLs)"; exit 1; }
for tgz in *.tar.gz; do
  [ -e "$tgz" ] || continue
  [ "$(stat -c%s "$tgz")" -lt 10000 ] && continue   # skip failed downloads
  dir="${tgz%.tar.gz}"
  name="${dir%%-*}"
  echo "=== $name ==="
  [ -d "$dir" ] || tar xzf "$tgz" 2>/dev/null
  ( cd "$dir" || exit 1
    if [ -x ./configure ]; then
      timeout 150 ./configure --host=i686-w64-mingw32 CFLAGS="-O2 -g0" >cfg.log 2>&1 || { echo "  configure FAIL"; exit 1; }
    fi
    timeout 300 make -j4 >make.log 2>&1
    # collect any produced .exe (prefer the main tool)
    found=$(find . -name "*.exe" -type f 2>/dev/null | grep -viE "test|conftest|check" | head -1)
    if [ -n "$found" ]; then cp "$found" "$BINS/$name.exe" && echo "  OK -> $name.exe ($(stat -c%s "$BINS/$name.exe") bytes)"; else echo "  no .exe (make tail:)"; tail -2 make.log; fi
  )
done
echo "=== BINS ==="; ls -la "$BINS"
