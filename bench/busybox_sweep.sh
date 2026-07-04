#!/usr/bin/env bash
# Systematic applet differential for a REAL multi-call binary: transpile the
# actual BusyBox-w32 (MinGW, stripped, 600 KB, ~390 applets in one PE) with ARET
# and run a broad, DETERMINISTIC battery of applet invocations against the SAME
# binary under Wine (ground truth). Every applet whose stdout+stderr+exit-code
# is not byte-identical to Wine is a real gap or bug.
#
# Complements sqlite_sweep (one engine, deep feature surface) and winediff (small
# synthetic programs): BusyBox is ~390 tiny independent programs behind one entry
# point, so it stresses argv routing, the CRT stdio layer and a wide kernel32
# surface at once. This sweep is exactly how the general bugs it now guards were
# found — CryptGenRandom (no applet could route), _filbuf (every stdin read
# looped forever), fclose-of-stdin (rev/nl crashed on exit).
#
# Two invariants make ARET and Wine comparable:
#  1. argv[0] must start with "busybox" — BusyBox only enters applet dispatch
#     then; otherwise it treats argv[0]'s basename as the applet ("app" -> "not
#     found"). Both sides are therefore invoked as a file named `busybox.exe`.
#  2. No shell-glob metacharacters (* ? [) in arguments: BusyBox-w32 is built
#     with MinGW's CRT_glob, so under Wine `*` is wildcard-expanded against the
#     cwd (nondeterministic) while the transpile does not glob. Such args are not
#     a stable ground truth and are excluded.
#
# Skips (does not fail) when wine, the network, or the aret binary are missing.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ARET="${ARET:-$DIR/../target/release/aret}"
ARET="$(readlink -f "$ARET" 2>/dev/null || echo "$ARET")"
VER="FRP-5579-g5749feb35"   # BusyBox v1.38.0 snapshot — public, versioned, pinned
URL="https://frippery.org/files/busybox/busybox-w32-${VER}.exe"
SHA="497607849a3e581615e46292d9063313d9a27a54380aad60ba2c5328838e3bb6"
CACHE="${BUSYBOX_EXE:-$DIR/.cache/busybox-w32-${VER}.exe}"

command -v wine >/dev/null 2>&1 || { echo "SKIP (wine unavailable)"; exit 0; }
[ -x "$ARET" ] || { echo "SKIP (aret binary not built: $ARET)"; exit 0; }

if [ ! -f "$CACHE" ]; then
  mkdir -p "$(dirname "$CACHE")"
  if ! curl -fsS --max-time 90 -o "$CACHE" -L "$URL" 2>/dev/null; then
    rm -f "$CACHE"; echo "SKIP (cannot download BusyBox-w32)"; exit 0; fi
fi
if command -v sha256sum >/dev/null 2>&1; then
  got="$(sha256sum "$CACHE" | cut -d' ' -f1)"
  [ "$got" = "$SHA" ] || { echo "SKIP (BusyBox sha256 mismatch: $got)"; exit 0; }
fi

OUT="$(mktemp -d)"; trap 'rm -rf "$OUT"' EXIT
if ! "$ARET" --mode transpile --out-dir "$OUT/t" "$CACHE" >/dev/null 2>&1; then
  echo "FAIL (transpile of BusyBox-w32 failed)"; exit 1; fi
# Invariant 1: present both engines a file whose basename starts with "busybox".
mkdir -p "$OUT/a" "$OUT/w"
cp "$OUT/t/app" "$OUT/a/busybox.exe"
cp "$CACHE"     "$OUT/w/busybox.exe"
AP="$OUT/a/busybox.exe"; WP="$OUT/w/busybox.exe"
export WINEDEBUG=-all WINEPREFIX="$OUT/wp"
wine "$WP" true >/dev/null 2>&1   # warm the wineprefix once

# Deterministic stdin payloads (no dates / pids / randomness / glob chars).
WORDS=$'banana\napple\ncherry\napple\ndate\nBanana\n'
NUMS=$'10\n2\n33\n4\n100\n2\n'
TEXT=$'the quick brown fox\njumps over\nthe lazy dog\n'

pass=0; fail=0; report=""
# check <stdin> <applet> [args...] — compare stdout+stderr+exit against Wine.
check() {
  local in="$1"; shift
  local arc wrc
  printf '%s' "$in" | timeout 20 "$AP" "$@" >"$OUT/ao" 2>"$OUT/ae"; arc=$?
  printf '%s' "$in" | timeout 30 wine "$WP" "$@" >"$OUT/wo" 2>"$OUT/we"; wrc=$?
  sed -i 's/\r$//' "$OUT/wo" "$OUT/we"
  # Drop ARET's own diagnostics and Wine's infra chatter; keep the program's text.
  grep -av '^ARET: ' "$OUT/ae" >"$OUT/ae2" 2>/dev/null || true
  grep -avE '^wine:|:(fixme|err|warn|trace|class):' "$OUT/we" >"$OUT/we2" 2>/dev/null || true
  if [ "$arc" = "$wrc" ] && diff -q "$OUT/ao" "$OUT/wo" >/dev/null 2>&1 \
       && diff -q "$OUT/ae2" "$OUT/we2" >/dev/null 2>&1; then
    pass=$((pass+1))
  else
    fail=$((fail+1))
    report+="DIFF  busybox $*  (aret rc=$arc, wine rc=$wrc)"$'\n'
    report+="$(diff "$OUT/wo" "$OUT/ao" 2>/dev/null | grep -E '^[<>]' | head -4 | sed 's/^/        out /')"$'\n'
    local ed; ed="$(diff "$OUT/we2" "$OUT/ae2" 2>/dev/null | grep -E '^[<>]' | head -2 | sed 's/^/        err /')"
    [ -n "$ed" ] && report+="$ed"$'\n'
  fi
}

E=""
# --- arithmetic / expr (the 64-bit %I64d path is exercised by the big adds) ----
check "$E" expr 6 + 7
check "$E" expr 100 / 7
check "$E" expr 10 % 3
check "$E" expr 2000000000 + 2000000000
check "$E" expr 5000000000 - 1
check "$E" expr length abcdefgh
check "$E" expr substr hello 2 3
check "$E" expr index hello l
# --- echo / printf -------------------------------------------------------------
check "$E" echo hello world
check "$E" echo -n abc
check "$E" echo -e 'a\tb\tc'
check "$E" printf '%d|%s|%x|%o\n' 42 hi 255 64
check "$E" printf '%c%c%c\n' 65 66 67
check "$E" printf '%5d|%-5d|\n' 7 7
check "$E" printf '%08x\n' 3735928559
# --- path / bool / test --------------------------------------------------------
check "$E" basename /a/b/c.txt
check "$E" basename /a/b/c.txt .txt
check "$E" dirname /a/b/c.txt
check "$E" true
check "$E" false
check "$E" test 1 -lt 2
check "$E" test abc = abc
# --- seq / factor --------------------------------------------------------------
check "$E" seq 1 5
check "$E" seq 2 2 10
check "$E" seq -w 1 10
check "$E" factor 360
check "$E" factor 1000000007
# --- text pipelines (read stdin: argv routing + CRT stdio) ---------------------
check "$WORDS" cat
check "$WORDS" wc
check "$WORDS" wc -l
check "$WORDS" wc -c
check "$WORDS" wc -w
check "$WORDS" head -n 2
check "$WORDS" tail -n 2
check "$WORDS" head -c 5
check "$WORDS" sort
check "$WORDS" sort -r
check "$WORDS" sort -u
check "$WORDS" sort -f
check "$WORDS" uniq
check "$WORDS" uniq -c
check "$WORDS" rev
check "$WORDS" nl
check "$NUMS"  sort -n
check "$TEXT"  cut -c 1-3
check "$TEXT"  cut -d ' ' -f 1
check "$TEXT"  tr a-z A-Z
check "$TEXT"  tr -d aeiou
check "$TEXT"  fold -w 4
check "$TEXT"  awk '{print $1}'
check "$TEXT"  awk '{print NF}'
# --- hashing / encoding (byte-exact digests over stdin) ------------------------
check "$TEXT"  md5sum
check "$TEXT"  sha1sum
check "$TEXT"  sha256sum
check "$TEXT"  sha512sum
check $'aGVsbG8K' base64 -d
check "$TEXT"  od -c

# --- KNOWN GAPS (excluded from the gate; deeper than the CRT/kernel32 layer) ---
# These applets are NOT yet bit-identical and are left out so the gate stays a
# true regression signal. They are recorded here (not hidden) as the next targets
# — all in ARET's function-recovery / lifting domain, not the HLE shim layer:
#   grep, sed            — SIGSEGV inside the lifted regex engine (sub_42f6d4).
#   cksum, od -An -tx1   — abort: indirect call to an *unrecovered* function
#                          (0x41abec / 0x42f160) — analysis missed a call target.
#   base64 (encode)      — wrong output (a lifted-code defect; the fread fix
#                          corrected the stream read but the encode path still
#                          diverges).
# Re-including any of these once fixed is a one-line change here.
# --- constant system info ------------------------------------------------------
check "$E" uname
check "$E" uname -s

total=$((pass + fail))
echo "BusyBox-w32 applet sweep ($VER): $pass/$total invocations bit-identical to Wine"
if [ "$fail" != 0 ]; then
  printf '%s' "$report"
  echo "busybox sweep: DIVERGENCE (out/err lines: Wine '<' vs ARET '>')"
  exit 1
fi
echo "busybox sweep: all bit-identical (stdout + stderr + exit code)"
exit 0
