#!/bin/bash
# SessionStart hook: prepare the toolchain so the ARET verification benches
# (recompile / differential / SMT) can run during the session. Roadmap §10.
set -euo pipefail

# Only run in the remote (Claude Code on the web) environment.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "${CLAUDE_PROJECT_DIR:-.}"

# Build (cached after first run; warms the release binary used by the benches).
cargo build --release

# Z3 is needed for the level-3 SMT proofs (bench/smt_rewrites.sh). Install only
# if missing so the hook stays fast and idempotent.
if ! command -v z3 >/dev/null 2>&1; then
  pip install --quiet z3-solver || true
fi

# gcc/cc (level-1 recompile + level-2 differential) are part of the base image;
# report if absent rather than failing the session.
command -v cc >/dev/null 2>&1 || echo "warning: no C compiler (cc) found — recompile/differential benches will not run" >&2

echo "ARET session ready: $(cargo --version), $(cc --version 2>/dev/null | head -1), z3 $(z3 --version 2>/dev/null || echo 'absent')"
