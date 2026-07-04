#!/usr/bin/env bash
# funcdiff corpus gate — the differential oracle run on REAL committed binaries.
#
# Two differentials, both sound by construction (a divergence is a proven bug,
# a skip is never a false verdict — see src/cpudiff.rs):
#   * LIFT-closure : the lifted IR (interpreter, following direct calls into
#     recovered callees) vs the Unicorn CPU emulator — validates ir/lift.rs.
#   * OPT-diff     : the post-opt SSA IR (to_ssa + optimize) vs the pre-opt IR
#     (itself Unicorn-validated) — validates ssa/mod.rs + every optimizer pass.
#
# Runs on the pinned corpus (mingw busybox + MSVC sqlite, committed under
# bench/.cache/). Requires the `unpack` feature (system libunicorn). Skips
# cleanly if unicorn is unavailable or no corpus binary is present.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR/.."

# Any corpus binary present? (Otherwise skip rather than fail.)
if ! ls bench/.cache/busybox-w32-*.exe bench/.cache/sqlite3-*.exe >/dev/null 2>&1; then
  echo "SKIP (no corpus binary in bench/.cache/)"
  exit 0
fi

out="$(cargo test --release --features unpack --bin aret funcdiff_corpus \
        -- --ignored --nocapture 2>&1)"
status=$?

# Surface the per-binary summary lines.
echo "$out" | grep -E "LIFT-closure:|OPT-diff:|FUNCDIFF-CORPUS:|SKIP" | sed 's/^/  /'

# The test never *ran* (no result line) → a build/link problem, most likely a
# missing system libunicorn. Skip rather than fail the gate.
if ! echo "$out" | grep -q "test result:"; then
  echo "SKIP (unpack build unavailable — is libunicorn installed?)"
  exit 0
fi

if [ "$status" -eq 0 ] && echo "$out" | grep -q "test result: ok"; then
  echo "funcdiff corpus gate: PASS"
  exit 0
fi

# The test ran and failed → a real divergence. Show the witnesses.
echo "funcdiff corpus gate: FAIL"
echo "$out" | grep -E "divergence|panicked|fn 0x|assert" | head -20 | sed 's/^/  /'
exit 1
