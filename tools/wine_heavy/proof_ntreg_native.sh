#!/usr/bin/env bash
# Heavy-form NATIVE proof for the real-ABI Nt* REGISTRY floor (doc 82 tranche 6). Compiles our
# floor wrappers (ntdll_ntreg.c) with native cc (Linux/glibc, -m32 -fshort-wchar) and links them
# with a driver + reference in-memory registry into a native ELF; runs it with NO Wine at runtime
# and checks the output against the known-correct values. Bit-identical => the real-ABI registry
# floor works in ARET's own build model (autonomous ELF), the same path the builder now wires.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-cc}"
WD="$(mktemp -d)"; trap 'rm -rf "$WD"' EXIT
F="-m32 -fshort-wchar -O0 -w -fno-pie"
$CC $F -c "$HERE/ntdll_ntreg.c" -o "$WD/ntreg.o"
$CC $F -no-pie "$HERE/proof_ntreg_native.c" "$WD/ntreg.o" -o "$WD/native" 2>/dev/null
EXPECT=$'create hr=0x00000000 disp=1\nset hr=0x00000000\nquery hr=0x00000000 rl=16 type=4 dlen=4 val=42\nsmall hr=0x80000005 rl=16\nreopen hr=0x00000000 val=42'
if diff <("$WD/native") <(printf '%s\n' "$EXPECT"); then
  echo "NATIVE PROOF PASS: real-ABI Nt* registry floor compiled by native cc == known Wine values (autonomous ELF, no Wine at runtime)"
else
  echo "NATIVE PROOF FAIL: divergence above"; exit 1
fi
