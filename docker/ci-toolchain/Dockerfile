# ARET CI oracle toolchain — the closed oracle stack, baked into one image.
#
# WHY THIS EXISTS. The ARET oracles (winediff, cpudiff, difftest, …) run against
# Wine as the ground truth. In the dev container they are provisioned at session
# start by .claude/hooks/session-start.sh; re-running that apt dance on every CI job
# would dominate the wall-clock time and depend on package availability at run time.
# Baking the exact same stack into a pinned image makes every oracle job start from a
# known, reproducible environment in seconds — and it is the structural cure for the
# shared-wineserver wedge seen in-session, because each CI job is a fresh container
# with a fresh WINEPREFIX (never cached), so there is no stale wineserver to inherit.
#
# PINNED to ubuntu:24.04 (noble) on purpose: that is the distro the measured Wine
# constants come from — wine-9.0 (Ubuntu 9.0~repack), the MODERN-theme reference the
# GUI work (KN-0112) was measured against. A newer Wine would silently move those
# constants; a REAL divergence must stay a finding, never an environment drift.
#
# This image carries ONLY the toolchain, never the repository. The workflow checks the
# repo out on top, so the image is stable and its layers cache in the registry.
FROM ubuntu:24.04

# The i386 multiarch is required by Wine (the Win32 oracle is 32-bit, like ARET's
# target) and by `gcc -m32` (the level-1/2 differential benches).
# libgd3:i386 MUST be requested explicitly and FIRST: the apt resolver otherwise
# refuses it as a transitive dep of wine's libgphoto2:i386 and the whole wine install
# aborts with "held broken packages" (observed after a container reset, 2026-08 —
# the lesson is encoded here so the image build does not re-learn it).
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends libgd3:i386 \
 && apt-get install -y --no-install-recommends \
      wine wine32:i386 \
      gcc-mingw-w64-i686 g++-mingw-w64-i686 \
      gcc-multilib g++-multilib \
      libunicorn-dev \
      libsdl2-dev:i386 \
      xvfb \
      z3 \
      clang lld llvm \
      build-essential pkg-config \
      git curl ca-certificates unzip tar xz-utils zstd \
      python3 python3-venv python3-pip \
 && rm -rf /var/lib/apt/lists/*

# Rust via rustup (stable, minimal). Cargo.lock is format v4 and needs a current
# Cargo — the distro cargo is too old, exactly as session-start.sh notes. Installed
# into a shared location so any CI user (the job may not run as root) can use it.
ENV RUSTUP_HOME=/opt/rustup \
    CARGO_HOME=/opt/cargo \
    PATH=/opt/cargo/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --default-toolchain stable \
 && chmod -R a+rwX /opt/rustup /opt/cargo

# A smoke check that the whole oracle stack is really present, so a broken image
# fails at BUILD time (loud) rather than silently skipping oracles at run time.
# Mirrors the "ARET session ready" banner the dev hook prints.
RUN set -e; \
    echo 'int main(void){return 0;}' | gcc -m32 -x c - -o /tmp/m32 && rm -f /tmp/m32; \
    i686-w64-mingw32-gcc --version >/dev/null; \
    pkg-config --exists unicorn; \
    PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --exists sdl2; \
    wine --version; \
    z3 --version; \
    clang --version >/dev/null; \
    command -v Xvfb >/dev/null; \
    cargo --version; \
    echo "ARET CI toolchain ready: $(wine --version), mingw ok, gcc-m32 ok, unicorn ok, sdl2 ok, $(cargo --version)"
