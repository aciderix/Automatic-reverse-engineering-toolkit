#!/usr/bin/env bash
# Build a clang MSVC-ABI C++ exception-handling fixture into a runnable 32-bit PE.
# Unlike the winecorpus fixtures (mingw), C++ EH needs the MSVC ABI: clang emits the
# real _CxxThrowException / __CxxFrameHandler3 machinery, lld-link produces the PE, and
# msvcrt (Wine's, and ARET's HLE) provides the runtime. The SAME PE runs under Wine
# (oracle) and ARET (under test) — so ARET's C++ EH is measured bit-for-bit vs Wine.
#   usage: build.sh SRC.cpp OUT.exe
set -eu
SRC="$1"; OUT="$2"; DIR="$(cd "$(dirname "$0")" && pwd)"; TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
for d in /usr/lib/i386-linux-gnu/wine/i386-windows /usr/lib/wine/i386-windows; do
  [ -f "$d/msvcrt.dll" ] && MSVCRT="$d/msvcrt.dll" && break
done
: "${MSVCRT:?msvcrt.dll not found (install wine 32-bit)}"
python3 "$DIR/gen_msvcrt_lib.py" "$MSVCRT" > "$TMP/msvcrt.def"
llvm-dlltool -m i386 --input-def "$TMP/msvcrt.def" --output-lib "$TMP/msvcrt.lib"
clang --target=i686-pc-windows-msvc -fms-extensions -Os -c "$SRC" -o "$TMP/f.obj"
clang --target=i686-pc-windows-msvc -c "$DIR/eh_support.c" -o "$TMP/s.obj"
lld-link /subsystem:console /entry:mainCRTStartup /nodefaultlib \
         /out:"$OUT" "$TMP/f.obj" "$TMP/s.obj" "$TMP/msvcrt.lib"
