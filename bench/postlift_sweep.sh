#!/usr/bin/env bash
# Post-lift sweep — measure the POST-LIFT wall across a corpus of real C++ PE32s.
#
#   bash bench/postlift_sweep.sh <corpus-dir> [max_binaries]
#
# The raw wall sweep (`wallsweep.sh`) is STATIC and does NOT auto-lift, so it always
# re-shows libstdc++ as the #1 import wall even though libstdc++ is already lift-covered
# (KN-0091). This tool measures what actually breaks AFTER the C++ runtime is lifted:
# for each C++ binary it transpiles WITH `--auto-lift` (libstdc++/libgcc/... lifted
# beside it), RUNS it, and compares byte-exact to Wine — exactly the jsoncpp method,
# aggregated. It is a MEASUREMENT (a coverage/soundness map), never a gate: every fix
# it motivates must still be proven by the differential oracles.
#
# 100% RAM: build trees go to $POSTLIFT_WORK (default /dev/shm), so the session disk
# allocation (which cannot hold the ~1.6GB corpus) is never touched; each per-binary
# tree is deleted right after. The corpus INPUT is read-only and may live on disk or shm.
#
# Driver problem (KN-0092): arbitrary binaries emit nothing without the right args. We
# AUTO-DISCOVER a deterministic driver: try a small set of invocations under Wine, twice
# each; the first that yields STABLE, non-empty stdout is the driver, used identically
# for ARET. Binaries with no such invocation are reported SKIP(no-driver), never a fake pass.
#
# Env knobs: ARET, POSTLIFT_WORK, POSTLIFT_TIMEOUT (per-binary, s), POSTLIFT_MAXDLL
# (skip binaries needing more than N non-system DLLs — avoids giant Qt/LLVM cascades).
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
ARET="${ARET:-$DIR/../target/release/aret}"
ARET="$(readlink -f "$ARET" 2>/dev/null || echo "$ARET")"
CORPUS="${1:-}"
MAXN="${2:-30}"
WORK="${POSTLIFT_WORK:-/dev/shm/aret-postlift}"
TIMEOUT="${POSTLIFT_TIMEOUT:-200}"
MAXDLL="${POSTLIFT_MAXDLL:-3}"

[ -n "$CORPUS" ] && [ -d "$CORPUS" ] || { echo "usage: bash bench/postlift_sweep.sh <corpus-dir> [max_binaries]"; exit 2; }
[ -x "$ARET" ] || { echo "postlift: aret binary absent ($ARET) — SKIP"; exit 0; }
command -v wine >/dev/null 2>&1 || { echo "postlift: wine absent — SKIP"; exit 0; }

mkdir -p "$WORK"
export WINEDEBUG="${WINEDEBUG:--all}"

# --- dll-path: symlink each corpus DLL under its REAL name (files are <hash>_<name>). --
# --auto-lift and Wine both resolve DLLs by real name; the corpus stores them hash-prefixed.
DPD="$WORK/dllpath"; mkdir -p "$DPD"
for f in "$CORPUS"/*.dll; do
  b="$(basename "$f")"; r="${b#*_}"
  [ -e "$DPD/$r" ] || ln -s "$f" "$DPD/$r" 2>/dev/null
done
export WINEPATH="$DPD"

extract_aret() { awk '/--- program output ---/{f=1;next} f{sub(/^  \| ?/,"");print}'; }
norm() { tr -d '\r'; }

# Non-system DLL count for a PE (heuristic: DLL-name strings minus the common OS set).
_sysre='KERNEL32|KERNELBASE|msvcrt|USER32|GDI32|ADVAPI32|SHELL32|ole32|OLEAUT32|WS2_32|COMDLG32|COMCTL32|SHLWAPI|IMM32|WINMM|VERSION|CRYPT32|bcrypt|ntdll|RPCRT4|SETUPAPI|WINSPOOL|dwmapi|uxtheme|api-ms|dbghelp|psapi|userenv|wsock32|iphlpapi|mpr\.dll|d3d|opengl32|gdiplus'
nonsys_dlls() { strings -n 6 "$1" 2>/dev/null | grep -iE '\.dll$' | grep -viE "$_sysre" | sort -u; }

# Candidate deterministic invocations, in priority order.
DRIVERS=( "--version" "-V" "--help" "-h" "-help" "" )

# Optional CURATED driver table: bench/postlift_drivers.tsv, lines "<name-substr>\t<args...>".
# A binary whose basename contains <name-substr> uses those args verbatim (skips discovery)
# — the reliable way to drive tools that need real inputs or emit only on stderr. Curated
# entries win; auto-discovery is the fallback.
DRIVER_TABLE="$DIR/postlift_drivers.tsv"
curated_driver() {   # echoes args (may be empty) and returns 0 on match, else 1
  [ -f "$DRIVER_TABLE" ] || return 1
  local base="$1" sub rest
  while IFS=$'\t' read -r sub rest; do
    case "$sub" in ''|'#'*) continue;; esac
    case "$base" in *"$sub"*) printf '%s' "$rest"; return 0;; esac
  done < "$DRIVER_TABLE"
  return 1
}

# Discover a driver: first invocation giving STABLE non-empty Wine stdout (run twice).
# Echoes the chosen invocation ('' for no-args) on fd1 and returns 0; returns 1 if none.
discover_driver() {
  local exe="$1" inv a b
  for inv in "${DRIVERS[@]}"; do
    local args=(); [ -z "$inv" ] || args=("$inv")
    a="$(cd "$CORPUS" && timeout 30 wine "$exe" "${args[@]}" 2>/dev/null | norm)"
    [ -n "$a" ] || continue
    b="$(cd "$CORPUS" && timeout 30 wine "$exe" "${args[@]}" 2>/dev/null | norm)"
    [ "$a" = "$b" ] || continue          # non-deterministic -> skip this invocation
    printf '%s' "$inv"; return 0
  done
  return 1
}

echo "POST-LIFT SWEEP — corpus=$CORPUS  (auto-lift + run vs Wine, RAM=$WORK, per-binary ${TIMEOUT}s, <=${MAXDLL} non-sys DLL)"
pass=0 diff=0 abort=0 nodrv=0 skip=0 total=0
diffs=(); aborts=()

for exe in "$CORPUS"/*.exe; do
  [ -f "$exe" ] || continue
  grep -qa "libstdc++-6.dll" "$exe" 2>/dev/null || continue     # C++ (GNU runtime) only
  ndll="$(nonsys_dlls "$exe" | grep -c .)"
  [ "$ndll" -le "$MAXDLL" ] || continue                          # skip heavy cascades
  total=$((total+1)); [ "$total" -gt "$MAXN" ] && { total=$((total-1)); break; }
  name="$(basename "$exe" | sed 's/^[0-9a-f]*_//')"

  drvlabel=""
  if crest="$(curated_driver "$name")"; then
    read -r -a dargs <<< "$crest"; drvlabel="curated:${crest:-noargs}"
  elif inv="$(discover_driver "$exe")"; then
    dargs=(); [ -z "$inv" ] || dargs=("$inv"); drvlabel="$drvlabel"
  else
    nodrv=$((nodrv+1)); printf '  %-28s SKIP(no deterministic driver)\n' "$name"; continue
  fi
  oracle="$(cd "$CORPUS" && timeout 30 wine "$exe" "${dargs[@]}" 2>/dev/null | norm)"

  out="$WORK/build"; rm -rf "$out"
  got="$(ARET_OBJCACHE="$WORK/oc" ARET_CLEAN_INTERMEDIATES=1 \
         timeout "$TIMEOUT" "$ARET" "$exe" --mode transpile --auto-lift --dll-path "$DPD" \
         --out-dir "$out" --run -- "${dargs[@]}" 2>"$WORK/aerr" | extract_aret | norm)"
  rc=$?
  reason="$(grep -iE 'aret_unimpl|aret_unmodelled|abort|panic|SIGSEGV|unresolved' "$WORK/aerr" | head -1 | cut -c1-70)"
  rm -rf "$out"

  if [ "$rc" = 124 ]; then abort=$((abort+1)); aborts+=("$name: TIMEOUT>${TIMEOUT}s"); printf '  %-28s ABORT(timeout)  [drv:%s]\n' "$name" "$drvlabel"; continue; fi
  if [ -z "$got" ] && [ -n "$reason" ]; then abort=$((abort+1)); aborts+=("$name: $reason"); printf '  %-28s ABORT  %s  [drv:%s]\n' "$name" "$reason" "$drvlabel"; continue; fi
  if [ "$oracle" = "$got" ]; then pass=$((pass+1)); printf '  %-28s PASS  [drv:%s]\n' "$name" "$drvlabel"; else
    diff=$((diff+1)); d="$(diff <(printf '%s\n' "$oracle") <(printf '%s\n' "$got") | head -1)"; diffs+=("$name [drv:${inv:-noargs}]: $d"); printf '  %-28s DIFF  [drv:%s]\n' "$name" "$drvlabel"; fi
done

echo
echo "=== POST-LIFT RESULT — ${total} C++ binaries driven: ${pass} PASS / ${diff} DIFF / ${abort} ABORT / ${nodrv} no-driver ==="
if [ "$diff" -gt 0 ]; then echo "-- DIFF (post-lift behavioural divergence — root-cause candidates) --"; printf '  %s\n' "${diffs[@]}"; fi
if [ "$abort" -gt 0 ]; then echo "-- ABORT (post-lift completeness gap) --"; printf '  %s\n' "${aborts[@]}"; fi
# Machine-readable summary line for the pipeline artifact.
echo "POSTLIFT_SUMMARY driven=${total} pass=${pass} diff=${diff} abort=${abort} nodriver=${nodrv}"
