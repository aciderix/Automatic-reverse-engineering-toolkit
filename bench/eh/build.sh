#!/usr/bin/env bash
# Build a clang MSVC-ABI C++/SEH exception fixture into a runnable 32-bit PE. clang emits
# the real MSVC EH (_CxxThrowException / __CxxFrameHandler3 / __except_handler3); mingw's
# import libs carry the correctly stdcall-decorated Win32/CRT stubs, and a small generated
# lib from Wine's msvcrt supplies __CxxFrameHandler3 (v3), which mingw lacks. The SAME PE
# runs under Wine (oracle) and ARET (under test) -> EH measured bit-for-bit vs Wine.
#   usage: build.sh SRC.{c,cpp} OUT.exe
set -eu
SRC="$1"; OUT="$2"; DIR="$(cd "$(dirname "$0")" && pwd)"; TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
MINGW=/usr/i686-w64-mingw32/lib
for d in /usr/lib/i386-linux-gnu/wine/i386-windows /usr/lib/wine/i386-windows; do
  [ -f "$d/msvcrt.dll" ] && WMSVCRT="$d/msvcrt.dll" && break
done
: "${WMSVCRT:?msvcrt.dll not found (install wine 32-bit)}"
python3 "$DIR/gen_msvcrt_lib.py" "$WMSVCRT" > "$TMP/wmsvcrt.def"
llvm-dlltool -m i386 --input-def "$TMP/wmsvcrt.def" --output-lib "$TMP/wmsvcrt.lib"
clang --target=i686-pc-windows-msvc -fms-extensions -Os -c "$SRC" -o "$TMP/f.obj"
clang --target=i686-pc-windows-msvc -c "$DIR/eh_support.c" -o "$TMP/s.obj"
lld-link /safeseh:no /subsystem:console /entry:mainCRTStartup /nodefaultlib /out:"$OUT" \
         "$TMP/f.obj" "$TMP/s.obj" "$MINGW/libmsvcrt.a" "$MINGW/libkernel32.a" "$TMP/wmsvcrt.lib"
