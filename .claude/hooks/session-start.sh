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

# Provision the closed oracle toolchain when any piece is absent. Idempotent: an
# already prepared image only runs the checks. The 32-bit i386 multiarch is required
# by wine (the Win32 oracle) and by `gcc -m32` (the level-1/2 differential benches).
needs_oracle_toolchain=false
for executable in wine i686-w64-mingw32-gcc i686-w64-mingw32-g++ zstd; do
  command -v "$executable" >/dev/null 2>&1 || needs_oracle_toolchain=true
done
pkg-config --exists unicorn 2>/dev/null || needs_oracle_toolchain=true
command -v rustup >/dev/null 2>&1 || needs_oracle_toolchain=true
# gcc-multilib ships no binary of its own — probe `gcc -m32` directly (difftest builds).
echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null || needs_oracle_toolchain=true
if [ "$needs_oracle_toolchain" = true ]; then
  sudo dpkg --add-architecture i386 >/dev/null 2>&1 || true
  sudo apt-get update >/dev/null 2>&1 || true
  # libgd3:i386 MUST be requested explicitly and first: the apt resolver otherwise
  # refuses it as a transitive dep of wine's libgphoto2:i386 and the whole wine install
  # aborts with "held broken packages" (observed after a container reset, 2026-08).
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libgd3:i386 >/dev/null 2>&1 || true
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    wine wine32:i386 \
    gcc-mingw-w64-i686 g++-mingw-w64-i686 \
    gcc-multilib g++-multilib \
    libunicorn-dev rustup zstd >/dev/null 2>&1 || \
    echo "warning: oracle toolchain provisioning was incomplete" >&2
fi

# Cargo.lock is format v4 and requires a current Cargo. Rustup keeps this
# independent of the older distro Cargo that may be pre-installed in a VM.
if command -v rustup >/dev/null 2>&1; then
  rustup toolchain install stable --profile minimal >/dev/null 2>&1 || true
  RUSTUP_CARGO="$(rustup which cargo 2>/dev/null || true)"
  RUSTUP_RUSTC="$(rustup which rustc 2>/dev/null || true)"
  if [ -n "$RUSTUP_CARGO" ] && [ -n "$RUSTUP_RUSTC" ]; then
    export PATH="$(dirname "$RUSTUP_CARGO"):$PATH"
    export RUSTC="$RUSTUP_RUSTC"
  else
    echo "warning: current Rust toolchain unavailable; Cargo.lock v4 may not build" >&2
  fi
fi

# Build (cached after first run; warms the release binary used by the benches).
cargo build --release

# ARET-MMU is declared by the project-level .mcp.json and runs from its own
# aret-memory/.venv. Warm that venv now (uv-first, ~1-2s) so the memory tools connect
# instantly on first use, even in a brand-new ephemeral container. Non-fatal and
# idempotent: the MCP launcher re-bootstraps the same venv on demand if this is skipped.
"${CLAUDE_PROJECT_DIR:-.}/aret-memory/scripts/bootstrap_venv.sh" || \
  echo "warning: ARET-MMU venv bootstrap incomplete (the MCP launcher will retry on demand)" >&2

# Z3 is needed for the level-3 SMT proofs (bench/smt_rewrites.sh). Install only
# if missing so the hook stays fast and idempotent.
if ! command -v z3 >/dev/null 2>&1; then
  pip install --quiet z3-solver || true
fi

# SDL2 (i386) backs the M7 GUI layer (doc 72): the transpiled output is a 32-bit
# ELF, so the GUI HLE links 32-bit SDL2. Install only if missing (idempotent, like
# z3). Non-fatal: the CLI / message-only / headless-content paths don't need it,
# and the GUI build detects SDL2 via pkg-config and degrades gracefully if absent.
if ! PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --exists sdl2 2>/dev/null; then
  sudo dpkg --add-architecture i386 >/dev/null 2>&1 || true
  sudo apt-get update >/dev/null 2>&1 || true
  sudo apt-get install -y libsdl2-dev:i386 >/dev/null 2>&1 || true
fi
# Xvfb: a virtual framebuffer X server so the GUI oracle (Wine CreateWindow) runs
# headless in winediff.sh. Install only if missing.
if ! command -v Xvfb >/dev/null 2>&1; then
  sudo apt-get install -y xvfb >/dev/null 2>&1 || true
fi

# gcc/cc (level-1 recompile + level-2 differential) are part of the base image;
# report if absent rather than failing the session.
command -v cc >/dev/null 2>&1 || echo "warning: no C compiler (cc) found — recompile/differential benches will not run" >&2

# Report the full oracle stack so a silently-incomplete reset is visible at a glance:
# wine (Win32 oracle), mingw (PE fixtures), `gcc -m32` (difftest), unicorn (cpudiff).
_m32=$(echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /dev/null 2>/dev/null && echo ok || echo ABSENT)
_mingw=$(command -v i686-w64-mingw32-gcc >/dev/null 2>&1 && echo ok || echo ABSENT)
_uni=$(pkg-config --exists unicorn 2>/dev/null && echo ok || echo ABSENT)
echo "ARET session ready: $(cargo --version), $(cc --version 2>/dev/null | head -1), z3 $(z3 --version 2>/dev/null || echo 'absent'), wine $(wine --version 2>/dev/null || echo 'absent'), mingw $_mingw, gcc-m32 $_m32, unicorn $_uni"
