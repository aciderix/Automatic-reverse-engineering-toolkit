#!/usr/bin/env bash
# Heavy-form NATIVE capstone proof (doc 82 tranche 6): compile Wine's dlls/ntdll/reg.c UNCHANGED
# (forward-decl splice only) with native cc + ARET's NT-types shim, link it with the real-ABI Nt*
# registry floor (ntdll_ntreg.c) + rtlstr.c + the ASCII floor + a driver/reference registry, run
# the resulting native ELF (NO Wine at runtime), and check the registry round-trip against the
# known Wine values. Bit-identical => a whole non-string Wine ntdll file runs on ARET's floor.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
TAG="${WINE_TAG:-wine-9.0}"; CC="${CC:-cc}"
command -v curl >/dev/null 2>&1 || { echo "SKIP (no curl)"; exit 0; }
WD="$(mktemp -d)"; trap 'rm -rf "$WD"' EXIT
for f in reg rtlstr; do
  curl -s --max-time 60 -o "$WD/$f.c" "https://raw.githubusercontent.com/wine-mirror/wine/$TAG/dlls/ntdll/$f.c" || { echo "SKIP (fetch $f.c failed)"; exit 0; }
  python3 - "$WD/$f.c" "$WD/${f}_fwd.c" <<PY
import sys; sys.path.insert(0,"$ROOT/tools"); import gen_wine_heavy as g
open(sys.argv[2],"w").write(g.splice_forward_decls(open(sys.argv[1]).read())[0])
PY
done
F="-m32 -fshort-wchar -O0 -w -fno-pie -fno-strict-aliasing -fno-stack-protector"
S="$ROOT/runtime/wine_heavy/native"
$CC $F -c -D__WINESRC__ -I "$S" "$WD/rtlstr_fwd.c" -o "$WD/rtlstr.o"
$CC $F -c -D__WINESRC__ -I "$S" "$WD/reg_fwd.c"    -o "$WD/reg.o"
$CC $F -c "$ROOT/runtime/wine_heavy/ntdll_floor.c" -o "$WD/floor.o"
$CC $F -c "$ROOT/runtime/wine_heavy/ntdll_ntreg.c" -o "$WD/ntreg.o"
$CC $F -no-pie -I "$S" "$HERE/proof_reg_native.c" "$WD/reg.o" "$WD/rtlstr.o" "$WD/floor.o" "$WD/ntreg.o" -o "$WD/native"
EXPECT=$'create hr=0x00000000 disp=1\nset hr=0x00000000\nquery hr=0x00000000 type=4 count=4 val=42'
if diff <("$WD/native") <(printf '%s\n' "$EXPECT"); then
  echo "CAPSTONE PROOF PASS: whole Wine reg.c compiled by native cc + real-ABI Nt* floor == known Wine values (autonomous ELF, no Wine at runtime)"
else
  echo "CAPSTONE PROOF FAIL: divergence above"; exit 1
fi
