#!/usr/bin/env bash
# Heavy-form PROOF for the real-ABI Nt* REGISTRY floor (doc 82 tranche 5). Compile our floor
# wrappers (ntdll_ntreg.c) and link them with a driver that calls the NT registry syscalls the way
# a compiled Wine ntdll .c would; run under Wine and diff the output against the SAME driver linked
# to Wine's REAL ntdll. Bit-identical => our floor implements the NT registry ABI correctly on top
# of an in-memory registry. Self-contained (no network); needs only wine + the mingw cross-gcc.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
GCC="${MINGW_GCC:-i686-w64-mingw32-gcc}"
command -v "$GCC" >/dev/null 2>&1 || { echo "SKIP (no $GCC)"; exit 0; }
command -v wine  >/dev/null 2>&1 || { echo "SKIP (no wine)"; exit 0; }
WD="$(mktemp -d)"; trap 'rm -rf "$WD"' EXIT
export WINEPREFIX="$WD/wp" WINEDEBUG=-all TZ=UTC LC_ALL=C
wine wineboot --init >/dev/null 2>&1 || true

$GCC -m32 -c -O1 -w "$HERE/ntdll_ntreg.c" -o "$WD/ntreg.o"
# ours: driver + our floor wrappers + the reference in-memory core (ARET_REFERENCE_CORE)
$GCC -m32 -O1 -w -DARET_REFERENCE_CORE "$HERE/proof_ntreg_driver.c" "$WD/ntreg.o" -o "$WD/ours.exe" 2>/dev/null
# oracle: the same driver on Wine's real ntdll
$GCC -m32 -O1 -w "$HERE/proof_ntreg_driver.c" -lntdll -o "$WD/oracle.exe" 2>/dev/null

if diff <(wine "$WD/ours.exe" 2>/dev/null) <(wine "$WD/oracle.exe" 2>/dev/null); then
  echo "PROOF PASS: real-ABI Nt* registry floor + in-memory registry == Wine real ntdll (bit-identical)"
else
  echo "PROOF FAIL: divergence above"; exit 1
fi
