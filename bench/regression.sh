#!/usr/bin/env bash
# Unified regression gate (roadmap §10): runs every level of the north-star
# metric and the test suite, and fails if any check regresses. Intended for use
# before a commit and from CI / a SessionStart hook.
#
#   bash bench/regression.sh
#
# Env: ARET (binary, default target/release/aret); SKIP_SYSBINS=1 to skip the
# system-binary recompilability checks (e.g. on a machine without /usr/bin/gzip).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ARET="${ARET:-target/release/aret}"

fail=0
step() { printf '\n=== %s ===\n' "$1"; }
ok()   { printf '  PASS  %s\n' "$1"; }
ko()   { printf '  FAIL  %s\n' "$1"; fail=1; }

step "build (release)"
if cargo build --release 2>&1 | tail -1; then ok "cargo build"; else ko "cargo build"; fi

step "unit tests"
if cargo test --release 2>&1 | grep -q "test result: ok"; then
  ok "cargo test"
else
  ko "cargo test"; cargo test --release 2>&1 | grep -E "FAILED|test result" | head
fi

step "differential equivalence (level 2)"
out="$(bash bench/difftest.sh 2>&1 | tail -1)"
echo "  $out"
n="${out##*: }"; got="${n%%/*}"; tot="${n#*/}"; tot="${tot%% *}"
if [ -n "$got" ] && [ "$got" = "$tot" ]; then ok "difftest $n"; else ko "difftest $out"; fi

step "in-place differential (globals/tables)"
out="$(bash bench/inplace.sh 2>&1 | tail -1)"
echo "  $out"
n="${out##*: }"; got="${n%%/*}"; tot="${n#*/}"; tot="${tot%% *}"
if [ -n "$got" ] && [ "$got" = "$tot" ]; then ok "in-place $n"; else ko "in-place $out"; fi

step "magic-division exhaustive 2^32"
out="$(bash bench/magicdiv.sh 2>&1 | tail -1)"
echo "  $out"
case "$out" in *EQUIVALENT) ok "magicdiv" ;; *) ko "magicdiv: $out" ;; esac

step "SMT proofs (level 3)"
if command -v "${Z3:-z3}" >/dev/null 2>&1; then
  res="$(bash bench/smt_rewrites.sh 2>&1)"
  if echo "$res" | grep -q FAILED; then ko "smt"; echo "$res" | grep FAILED; else
    ok "smt $(echo "$res" | grep -c PROVED)/11 proved"
  fi
else
  echo "  SKIP  z3 not installed"
fi

step "recompilability (level 1)"
if [ "${SKIP_SYSBINS:-0}" = 1 ]; then
  echo "  SKIP  SKIP_SYSBINS=1"
else
  for b in /usr/bin/gzip /bin/ls /bin/cat; do
    [ -f "$b" ] || continue
    cp "$b" /tmp/.aret_rt 2>/dev/null || continue
    line="$("$ARET" /tmp/.aret_rt --mode verify --limit 200 2>/dev/null | grep -oE '[0-9]+/[0-9]+ functions \([0-9.]+%\)')"
    name="$(basename "$b")"
    case "$line" in
      *"(100.0%)") ok "$name $line" ;;
      "") ko "$name (no output)" ;;
      *) ko "$name $line" ;;
    esac
  done
  rm -f /tmp/.aret_rt
fi

echo
if [ "$fail" = 0 ]; then echo "REGRESSION GATE: PASS"; else echo "REGRESSION GATE: FAIL"; fi
exit "$fail"
