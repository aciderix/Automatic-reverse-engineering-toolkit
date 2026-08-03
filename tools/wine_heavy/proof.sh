#!/usr/bin/env bash
# Heavy-form PROOF (doc 82): compile Wine's dlls/ntdll/rtlstr.c UNCHANGED, link it against
# our 12-primitive ASCII floor, run under Wine, and diff the output against the SAME driver
# linked to Wine's REAL ntdll. Bit-identical => the compiled Wine functions run correctly on
# our ported floor. Reproducible; nothing here is wired into the ARET binary yet.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
TAG="${WINE_TAG:-wine-9.0}"; GCC=i686-w64-mingw32-gcc
WD="$(mktemp -d)"; trap 'rm -rf "$WD"' EXIT
curl -s --max-time 60 -o "$WD/rtlstr.c" \
  "https://raw.githubusercontent.com/wine-mirror/wine/$TAG/dlls/ntdll/rtlstr.c"
python3 - "$WD/rtlstr.c" "$WD/rtlstr_fwd.c" <<PY
import sys; sys.path.insert(0,"$ROOT/tools"); import gen_wine_heavy as g
open(sys.argv[2],"w").write(g.splice_forward_decls(open(sys.argv[1]).read())[0])
PY
$GCC -m32 -c -O1 -w -std=gnu11 -I "$HERE" -isystem /usr/i686-w64-mingw32/include \
  -D__WINESRC__ "$WD/rtlstr_fwd.c" -o "$WD/rtlstr.o"
$GCC -m32 -c -O1 -w "$HERE/ntdll_floor.c" -o "$WD/floor.o"
$GCC -m32 -O1 -w "$HERE/proof_driver.c" "$WD/rtlstr.o" "$WD/floor.o" -o "$WD/ours.exe" 2>/dev/null
$GCC -m32 -O1 -w "$HERE/proof_driver.c" -lntdll -o "$WD/oracle.exe" 2>/dev/null
if diff <(WINEDEBUG=-all wine "$WD/ours.exe" 2>/dev/null) \
        <(WINEDEBUG=-all wine "$WD/oracle.exe" 2>/dev/null); then
  echo "PROOF PASS: compiled Wine rtlstr.o + ASCII floor == Wine real ntdll (bit-identical)"
else
  echo "PROOF FAIL: divergence above"; exit 1
fi
