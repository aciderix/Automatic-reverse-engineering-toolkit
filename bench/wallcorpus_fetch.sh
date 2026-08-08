#!/usr/bin/env bash
# Build a measurement corpus of real PE32 x86 binaries and (optionally) run the
# wall sweep on it. This is the Levier-0 tool of doc 70 §5.0 / doc 90: gather a
# large, DIVERSE set of real compiled binaries so `wallsweep.sh` ranks the coverage
# gaps by #binaries blocked — priorities come from the data, never intuition.
#
#   bash bench/wallcorpus_fetch.sh <out-dir> [N_PACKAGES]
#   ARET=./corpus/aret_to.sh bash bench/wallsweep.sh <out-dir>/pe32   # then measure
#
# Sources (all FOSS, redistributable; reachability verified via the agent proxy
# 2026-08-08 — see doc 90):
#   - MSYS2 mingw32   (repo.msys2.org)  -> thousands of GCC/Clang PE32 (the bulk)
#   - UnxUtils        (SourceForge)     -> ~119 MSVC6 EXE (compiler diversity)
# The corpus binaries themselves are NOT committed (size + they are trivially
# re-fetchable); this script IS the reproducible artifact.
set -u
OUT="${1:?usage: wallcorpus_fetch.sh <out-dir> [N_PACKAGES]}"
NPKG="${2:-450}"
mkdir -p "$OUT"/{pe32,tmp,logs}
BASE="https://repo.msys2.org/mingw/mingw32/"

# --- one package: download, extract mingw32/bin, keep PE32 x86, dedup by sha256 ---
one() {
  local pkg="$1" out="$2" base="https://repo.msys2.org/mingw/mingw32/"
  local t="$out/tmp/$pkg"; mkdir -p "$t"
  curl -sS -m 90 -L "$base$pkg" -o "$t/a.zst" 2>/dev/null || { rm -rf "$t"; return; }
  tar --use-compress-program=unzstd -xf "$t/a.zst" -C "$t" 'mingw32/bin' 2>/dev/null || { rm -rf "$t"; return; }
  local f desc sha dst
  for f in "$t"/mingw32/bin/*.exe "$t"/mingw32/bin/*.dll; do
    [ -f "$f" ] || continue
    desc=$(file -b "$f" 2>/dev/null)
    case "$desc" in *"PE32 executable"*"80386"*) ;; *) continue ;; esac
    case "$desc" in *"Mono"*|*".Net"*) continue ;; esac   # skip .NET
    sha=$(sha256sum "$f" | cut -c1-16); dst="$out/pe32/${sha}_$(basename "$f")"
    [ -e "$dst" ] || cp "$f" "$dst"
  done
  echo "OK $pkg"; rm -rf "$t"
}
export -f one

echo "== fetching MSYS2 mingw32 index =="
curl -sS -m 60 "$BASE" | grep -oE 'mingw-w64-i686-[^"]+\.pkg\.tar\.zst' | grep -v '\.sig' | sort -u > "$OUT/logs/all.txt"
python3 - "$OUT/logs/all.txt" "$OUT/logs/sample.txt" "$NPKG" <<'PY'
import re,sys
from collections import OrderedDict
pk=open(sys.argv[1]).read().split(); latest=OrderedDict()
for p in pk:
    m=re.match(r'(mingw-w64-i686-.+?)-(\d[^-]*(?:-[^-]*)*)-any\.pkg\.tar\.zst$',p)
    if m: latest[m.group(1)]=p
u=list(latest.values()); k=max(1,len(u)//int(sys.argv[3]))
open(sys.argv[2],'w').write('\n'.join(u[::k])+'\n')
print(f"  {len(pk)} pkgs -> {len(u)} unique -> {len(u[::k])} sampled")
PY

echo "== downloading + extracting (10-way) =="
cat "$OUT/logs/sample.txt" | xargs -P 10 -I{} bash -c 'one "$@"' _ {} "$OUT" > "$OUT/logs/fetch.log" 2>&1

echo "== UnxUtils (MSVC6) =="
u="$OUT/tmp/unx"; mkdir -p "$u"
if curl -sS -m 120 -L "https://downloads.sourceforge.net/project/unxutils/unxutils/current/UnxUtils.zip" -o "$u/x.zip" 2>/dev/null; then
  ( cd "$u" && unzip -q -o x.zip 2>/dev/null )
  find "$u" -type f \( -iname '*.exe' -o -iname '*.dll' \) | while read -r f; do
    case "$(file -b "$f" 2>/dev/null)" in *"PE32 executable"*"80386"*) ;; *) continue ;; esac
    sha=$(sha256sum "$f"|cut -c1-16); cp -n "$f" "$OUT/pe32/${sha}_$(basename "$f")" 2>/dev/null
  done
fi
rm -rf "$OUT/tmp"

# single-exe timeout wrapper for wallsweep (huge DLLs shouldn't stall the sweep)
cat > "$OUT/aret_to.sh" <<EOF
#!/bin/bash
exec timeout 45 "$(pwd)/target/release/aret" "\$@"
EOF
chmod +x "$OUT/aret_to.sh"

echo "== corpus ready: $(ls "$OUT/pe32" | wc -l) PE32 binaries in $OUT/pe32 =="
echo "   measure: ARET=$OUT/aret_to.sh bash bench/wallsweep.sh $OUT/pe32"
