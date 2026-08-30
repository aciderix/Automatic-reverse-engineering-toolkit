#!/usr/bin/env bash
# Provision zlib1.dll (and the dev files to compile against it) into bench/.cache/zlib
# so the zlib_* winediff fixtures can prove ARET's zlib lifting. The DLL is NOT committed
# (re-fetchable); THIS script is the reproducible artifact. Idempotent.
#
#   bash bench/zlib_fetch.sh            # populate bench/.cache/zlib
#   bash bench/winediff.sh zlib_roundtrip
#
# Source: MSYS2 mingw32 (repo.msys2.org). zlib1.dll's only non-Windows dep is
# libgcc_s_dw2-1.dll, which ships with the mingw toolchain (a .winelibs entry).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/.cache/zlib"; dev="$out/dev"
mkdir -p "$out" "$dev/lib"
BASE="https://repo.msys2.org/mingw/mingw32/"

if [ -f "$out/zlib1.dll" ] && [ -f "$dev/include/zlib.h" ] && [ -f "$dev/lib/libz.dll.a" ]; then
  echo "== zlib runtime already present in $out =="; ls "$out"; exit 0
fi

pk="$(curl -sS -m 60 "$BASE" | grep -oE 'mingw-w64-i686-zlib-[0-9][^"]+\.pkg\.tar\.zst' \
      | grep -v '\.sig' | sort -u | tail -1)"
[ -n "$pk" ] || { echo "ERREUR : paquet zlib introuvable"; exit 1; }
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -sS -m 180 -L "$BASE$pk" -o "$tmp/z.zst" 2>/dev/null \
  && tar --use-compress-program=unzstd -xf "$tmp/z.zst" -C "$tmp" \
       'mingw32/bin' 'mingw32/include' 'mingw32/lib' 2>/dev/null \
  || { echo "ERREUR : extraction $pk"; exit 1; }

cp "$tmp/mingw32/bin/zlib1.dll" "$out/" 2>/dev/null
mkdir -p "$dev/include"
cp "$tmp/mingw32/include/zlib.h" "$tmp/mingw32/include/zconf.h" "$dev/include/" 2>/dev/null
cp "$tmp/mingw32/lib/libz.dll.a" "$dev/lib/" 2>/dev/null

miss=""
[ -f "$out/zlib1.dll" ]        || miss="$miss zlib1.dll"
[ -f "$dev/include/zlib.h" ]   || miss="$miss zlib.h"
[ -f "$dev/lib/libz.dll.a" ]   || miss="$miss libz.dll.a"
[ -n "$miss" ] && { echo "== INCOMPLET, manque :$miss =="; exit 1; }
echo "== zlib runtime ready in $out ($pk) =="; ls "$out"
