#!/usr/bin/env bash
# ABI audit — every __stdcall shim we implement must have its `@N` in stdcall_pops.
#
# Why this exists: a missing `@N` makes every caller of that import leave esp N bytes
# low, silently, for the rest of the function. It is the most expensive bug family in
# this project (cksum, 7za, the `0xe` wall, three separate /GS walls on WinMerge), and
# NONE of the other gates can see it — difftest, cpudiff and funcdiff never call an
# import, so only a real binary exercising the API reveals the drift. A one-off audit
# found 35 latent ones at once (2026-07-26); this script keeps the class from coming
# back.
#
# Ground truth is the decoration mingw's import libraries carry (`_Name@N`), the same
# source doc 70 §4.3 uses — not a guess, not a header, not documentation.
#
# Skips (does not fail) when the mingw cross-toolchain is unavailable, like the other
# bench harnesses gate on their toolchains.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/.." && pwd)"
NM="${NM:-i686-w64-mingw32-nm}"
LIBDIR="${MINGW_LIBDIR:-/usr/i686-w64-mingw32/lib}"

command -v "$NM" >/dev/null 2>&1 || { echo "SKIP: $NM not found"; exit 0; }
libs=()
for l in kernel32 user32 gdi32 shlwapi advapi32 comctl32 ole32 oleaut32 \
         version comdlg32 shell32 winspool; do
  [ -f "$LIBDIR/lib$l.a" ] && libs+=("$LIBDIR/lib$l.a")
done
[ "${#libs[@]}" -eq 0 ] && { echo "SKIP: no mingw import libraries under $LIBDIR"; exit 0; }

"$NM" --defined-only "${libs[@]}" 2>/dev/null \
  | grep -oE '^[0-9a-f]* T _[A-Za-z_0-9]+@[0-9]+' | sed 's/.* T _//' | sort -u > "$DIR/.gt_pops"

python3 - "$ROOT" "$DIR/.gt_pops" <<'PY'
import re, sys
root, gtfile = sys.argv[1], sys.argv[2]

gt = {}
for line in open(gtfile):
    name, _, pops = line.strip().rpartition('@')
    if name:
        gt[name] = int(pops)

shims = set()
for f in ('aret_hle.c', 'aret_win32.c', 'aret_crt.c'):
    src = open(f'{root}/runtime/aret_hle/{f}').read()
    shims |= set(re.findall(r'^uint32_t aret_([A-Za-z_0-9]+)\(uint32_t esp\)', src, re.M))

table = set(re.findall(r'\("([A-Za-z_0-9]+)",\s*\d+\)',
                       open(f'{root}/src/ir/stdcall_pops.rs').read()))

stdcall = sorted(n for n in shims if gt.get(n, 0) > 0)
missing = [n for n in stdcall if n not in table]

print(f"shims implemented            : {len(shims)}")
print(f"proven __stdcall (ground truth): {len(stdcall)}")
print(f"missing @N in stdcall_pops   : {len(missing)}")
for n in missing:
    print(f"  MISSING  {n}@{gt[n]}  -> every caller drifts esp by {gt[n]} bytes")
print("------------------------------------------")
if missing:
    print(f"stdcall-pop audit: FAIL ({len(missing)} latent esp drift(s))")
    sys.exit(1)
print("stdcall-pop audit: PASS")
PY
rc=$?
rm -f "$DIR/.gt_pops"
exit $rc
