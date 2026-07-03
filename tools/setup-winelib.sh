#!/usr/bin/env bash
# Reproducibly provision the Winelib toolchain (winegcc/winebuild + 32-bit import
# libs + Windows headers) needed to compile ARET's transpiled C against Wine's
# NATIVE Win32 implementations (see docs/vision/60-doctrine-reutilisation-verifiee.md).
#
# Why a script: winegcc is a system tool, not vendored in the repo. On a clean
# Debian/Ubuntu `apt install` works; in constrained containers the wine metapackage
# pulls a broken dep chain (libgphoto2 -> libgd3), so we fall back to downloading
# the .deb files and extracting just the tool + import libs (no runtime deps).
#
# Verifies at the end by compiling a tiny Win32 program to a native ELF.
set -euo pipefail

need() { command -v "$1" >/dev/null 2>&1; }

if need winegcc-stable && [ -e /usr/lib/i386-linux-gnu/wine/i386-unix/libkernel32.a ]; then
    echo "winelib toolchain already present."
else
    echo "== 1) clean path: apt install =="
    apt-get update -qq || true
    if apt-get install -y --no-install-recommends wine32-tools libwine-dev:i386 2>/dev/null \
       && need winegcc-stable; then
        echo "installed via apt."
    else
        echo "== 2) fallback: download .deb + extract files only (deps blocked) =="
        WORK="$(mktemp -d)"; cd "$WORK"
        apt-get download wine32-tools:i386 libwine-dev:i386
        for d in *.deb; do dpkg -x "$d" root/; done
        # tools (winegcc/winebuild) + their -stable wrappers
        cp -a root/usr/lib/wine/winegcc root/usr/lib/wine/winebuild /usr/lib/wine/
        cp -a root/usr/bin/winegcc-stable root/usr/bin/winebuild-stable /usr/bin/
        # Windows headers
        cp -a root/usr/include/wine /usr/include/
        # 32-bit ELF import libs, where winegcc -m32 looks for -lkernel32/-lntdll/-lwinecrt0
        cp -a root/usr/lib/i386-linux-gnu/wine /usr/lib/i386-linux-gnu/
        cd - >/dev/null; rm -rf "$WORK"
    fi
fi

echo "== verify: compile a Win32 program to a native ELF =="
T="$(mktemp -d)"; cd "$T"
cat > t.c <<'EOF'
#include <windows.h>
#include <stdio.h>
int main(void){ OSVERSIONINFOA o; o.dwOSVersionInfoSize=sizeof o; GetVersionExA(&o);
  printf("winelib-ok ver=%lu.%lu\n",o.dwMajorVersion,o.dwMinorVersion); return 0; }
EOF
winegcc-stable -m32 t.c -o t -L/usr/lib/i386-linux-gnu/wine/i386-unix
file t.exe.so | grep -q "ELF 32-bit" && echo "OK: native ELF produced" || { echo "FAIL: not a native ELF"; exit 1; }
WINEDEBUG=-all wine t.exe.so 2>/dev/null | grep -q winelib-ok && echo "OK: runs, Win32 APIs served natively by Wine" || echo "WARN: built but run needs a wine prefix"
cd - >/dev/null; rm -rf "$T"
echo "winelib toolchain ready. winegcc-stable -m32 <src> -o <out> -L/usr/lib/i386-linux-gnu/wine/i386-unix"
