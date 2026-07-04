#!/bin/bash
# SessionStart hook: prepare the toolchain so the ARET verification benches
# (recompile / differential / SMT) can run during the session. Roadmap §10.
set -euo pipefail

# Only run in the remote (Claude Code on the web) environment.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "${CLAUDE_PROJECT_DIR:-.}"

# Resync the checkout to origin BEFORE building, so a restored/stale container
# never resumes on an old base (the ephemeral container can come back behind what
# a later session already pushed). Safe by construction: it only fast-forwards a
# CLEAN working tree that is a strict ancestor of origin. If there are local
# commits not on origin, or uncommitted changes, it leaves everything untouched
# and warns — it never discards local work.
branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo '')"
if [ -n "$branch" ] && [ "$branch" != "HEAD" ]; then
  git fetch origin "$branch" 2>/dev/null || true
  if git rev-parse --verify "origin/$branch" >/dev/null 2>&1; then
    if [ -n "$(git status --porcelain)" ]; then
      echo "git: working tree has uncommitted changes — skipping auto-sync" >&2
    else
      local_head="$(git rev-parse HEAD)"
      origin_head="$(git rev-parse "origin/$branch")"
      if [ "$local_head" != "$origin_head" ]; then
        if git merge-base --is-ancestor "$local_head" "$origin_head"; then
          git checkout -B "$branch" "origin/$branch" >/dev/null 2>&1 \
            && echo "git: resynced $branch to origin ($origin_head)"
        else
          echo "git: local $branch has commits not on origin — NOT auto-syncing (reconcile manually)" >&2
        fi
      fi
    fi
  fi
fi

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
