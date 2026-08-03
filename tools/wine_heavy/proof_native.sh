#!/usr/bin/env bash
# Heavy-form NATIVE proof (doc 82): the same as proof.sh, but through ARET's REAL build model.
# ARET's HLE compiles with native `cc` (Linux/glibc) to a native ELF -- NOT mingw -- so a Wine
# ntdll .c needs (a) a self-contained NT-types layer (Linux has no winnt.h), (b) -fshort-wchar
# (native wchar_t is 32-bit; Windows WCHAR is 16-bit), (c) 16-bit wcslen/wcschr in the floor
# (glibc's are 32-bit). This compiles Wine's rtlstr.c with native cc + tools/wine_heavy/native/,
# links the ASCII floor, runs the resulting native ELF (no Wine at runtime), and checks the
# output against the known-correct values. Bit-identical => the heavy form works in ARET's own
# build, fully autonomous.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"
TAG="${WINE_TAG:-wine-9.0}"; CC="${CC:-cc}"
WD="$(mktemp -d)"; trap 'rm -rf "$WD"' EXIT
curl -s --max-time 60 -o "$WD/rtlstr.c" \
  "https://raw.githubusercontent.com/wine-mirror/wine/$TAG/dlls/ntdll/rtlstr.c"
python3 - "$WD/rtlstr.c" "$WD/rtlstr_fwd.c" <<PY
import sys; sys.path.insert(0,"$ROOT/tools"); import gen_wine_heavy as g
open(sys.argv[2],"w").write(g.splice_forward_decls(open(sys.argv[1]).read())[0])
PY
F="-m32 -fshort-wchar -O0 -w -fno-pie"
$CC $F -c -I "$HERE/native" -D__WINESRC__ "$WD/rtlstr_fwd.c" -o "$WD/rtlstr.o"
$CC $F -c "$HERE/ntdll_floor.c" -o "$WD/floor.o"
$CC $F -no-pie -I "$HERE/native" "$HERE/proof_driver_native.c" "$WD/rtlstr.o" "$WD/floor.o" -o "$WD/native" 2>/dev/null
EXPECT=$'initA len=5 max=6 str=Hello\na2u len=10 "Hello"\nu2a len=5 str=Hello\nint2char=ABCD\nequal(ci)=1 equal(cs)=0'
if diff <("$WD/native") <(printf '%s\n' "$EXPECT"); then
  echo "NATIVE PROOF PASS: Wine rtlstr.c compiled by native cc + ASCII floor == Wine (autonomous ELF, no Wine at runtime)"
else
  echo "NATIVE PROOF FAIL: divergence above"; exit 1
fi
