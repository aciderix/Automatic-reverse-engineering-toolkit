#!/usr/bin/env bash
# Axis 2 — OS-API (HLE / Winelib) differential. Complements axis 1 (cpudiff,
# CPU-translation correctness): here we check that ARET's high-level emulation of
# the Win32/CRT calls a program makes matches the *ground truth* of running the
# real PE under Wine.
#
# For each program in bench/winecorpus/:
#   1. build it to a 32-bit PE with mingw,
#   2. ORACLE: run the PE under Wine, capture stdout,
#   3. ARET:   transpile the PE to a native ELF and run it, capture stdout,
#   4. compare (line endings normalised).
# Reports pass/total = a concrete, chiffrée measure of OS-API coverage, and the
# divergences point straight at the missing/incorrect shims.
#
# Skips (does not fail) when Wine or the mingw cross-compiler is unavailable, like
# the other bench harnesses gate on their toolchains.
set -u
ARET="${ARET:-target/release/aret}"
# Absolute path: the run loop cd's into the temp dir (so program-created files
# land there), which would break a relative ARET path.
ARET="$(readlink -f "$ARET" 2>/dev/null || echo "$ARET")"
DIR="$(cd "$(dirname "$0")" && pwd)"
CORPUS="$DIR/winecorpus"
TMP="$(mktemp -d)"
XVFB_PID=""
cleanup() { [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
# Headless X for GUI fixtures: CreateWindow needs a display driver, so start one
# Xvfb for the whole run and point DISPLAY at it. Harmless for console fixtures.
# Absent Xvfb → GUI fixtures that need a real window simply can't create one (they
# stay honest: the same failure under Wine and ARET). One server, not per-test.
if command -v Xvfb >/dev/null 2>&1; then
  Xvfb :99 -screen 0 1280x1024x24 >/dev/null 2>&1 &
  XVFB_PID=$!
  export DISPLAY=:99
  sleep 0.4
fi
MINGW="${MINGW:-i686-w64-mingw32-gcc}"
WINDRES="${WINDRES:-${MINGW%-gcc}-windres}"
# Wine's PE builtin DLL dir (for `NAME.withdll` fixtures that lift a system DLL
# like comctl32 alongside the app — the same DLL Wine loads as the oracle).
WINE_PE_DIR=""
for d in /usr/lib/i386-linux-gnu/wine/i386-windows /usr/lib/wine/i386-windows \
         /opt/wine-stable/lib/wine/i386-windows; do
  [ -d "$d" ] && WINE_PE_DIR="$d" && break
done

if ! command -v "$MINGW" >/dev/null 2>&1; then
  echo "SKIP  ($MINGW unavailable; install mingw-w64)"; exit 0
fi
if ! command -v wine >/dev/null 2>&1; then
  echo "SKIP  (wine unavailable; install wine)"; exit 0
fi

# Isolated, quiet Wine prefix; initialise it once up front.
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEPREFIX="$TMP/wineprefix"
# Fixed timezone + locale so date/time/codepage conversions are deterministic.
export TZ=UTC
export LC_ALL=C
wine wineboot --init >/dev/null 2>&1 || true

# Program output of `aret --mode transpile --run` is delimited by a marker, each
# line prefixed "  | ".
extract_aret() { awk '/--- program output ---/{f=1;next} f{sub(/^  \| ?/,"");print}'; }
norm() { tr -d '\r'; }   # ignore CRLF-vs-LF line-ending differences

pass=0; total=0
for src in "$CORPUS"/*.c; do
  # Companion DLL sources (NAME.dll.c) are built by their app fixture, not run
  # standalone — skip them in the main loop.
  case "$src" in *.dll.c) continue;; esac
  name="$(basename "$src" .c)"; total=$((total+1))
  # Optional Windows resource (NAME.rc): compiled with windres and linked in, so
  # tests can embed resources (e.g. a VS_VERSIONINFO block for the version APIs).
  res_obj=""
  if [ -f "$CORPUS/$name.rc" ] && command -v "$WINDRES" >/dev/null 2>&1; then
    # -I CORPUS so an .rc can reference sibling files (e.g. a BITMAP "foo.bmp").
    "$WINDRES" -I "$CORPUS" "$CORPUS/$name.rc" -O coff -o "$TMP/$name.res.o" 2>"$TMP/err" || \
      { echo "FAIL  $name (windres: $(head -1 "$TMP/err"))"; continue; }
    res_obj="$TMP/$name.res.o"
  fi
  # Link the common Win32 libs a guard might reference (version info, OLE/COM,
  # BSTR, common controls). Harmless for programs that use none — the imports are
  # demand-loaded.
  # Optional per-program compile flags (winecorpus/NAME.cflags, whitespace-separated)
  # — e.g. -mstackrealign to exercise the GCC stack-realignment prologue.
  xcflags=""; [ -f "$CORPUS/$name.cflags" ] && xcflags="$(cat "$CORPUS/$name.cflags")"
  # Optional per-program import def (winecorpus/NAME.def): built into an import lib
  # with dlltool and linked *first*, so a fixture can force an import that the named
  # system libs would otherwise provide by name — e.g. an import BY ORDINAL (comctl32
  # InitCommonControls @17). Placed right after $src so it wins the symbol.
  imp_lib=""
  if [ -f "$CORPUS/$name.def" ] && command -v "${MINGW%-gcc}-dlltool" >/dev/null 2>&1; then
    if "${MINGW%-gcc}-dlltool" -d "$CORPUS/$name.def" -l "$TMP/$name.imp.a" 2>"$TMP/err"; then
      imp_lib="$TMP/$name.imp.a"
    else
      echo "FAIL  $name (dlltool: $(head -1 "$TMP/err"))"; continue
    fi
  fi
  # Optional companion DLL (winecorpus/NAME.dll.c): built as a real PE DLL the app
  # imports from; ARET lifts it too via `--with-dll` (DLL lifting, doc 80 §1.2) so
  # the app's imports of its exports dispatch to lifted code, not an HLE shim. The
  # DLL sits in $TMP next to the app, so Wine (the oracle) loads it normally.
  withdll=()
  if [ -f "$CORPUS/$name.dll.c" ]; then
    dllname="${name}dll.dll"
    if ! "$MINGW" -O1 -w -shared "$CORPUS/$name.dll.c" -o "$TMP/$dllname" \
         -Wl,--out-implib,"$TMP/$name.dllimp.a" 2>"$TMP/err"; then
      echo "FAIL  $name (DLL build: $(head -1 "$TMP/err"))"; continue
    fi
    imp_lib="$imp_lib $TMP/$name.dllimp.a"       # link the app against the DLL
    withdll=(--with-dll "$dllname=$TMP/$dllname") # and lift the DLL under ARET
  fi
  # Optional system-DLL lifting (winecorpus/NAME.withdll): one DLL name per line
  # (e.g. `comctl32.dll`) that ARET lifts from Wine's own PE builtins — the app
  # links against them normally and Wine (oracle) loads the same DLLs. Skips the
  # fixture if the builtin dir or a named DLL is missing (like a toolchain gate).
  if [ -f "$CORPUS/$name.withdll" ]; then
    if [ -z "$WINE_PE_DIR" ]; then echo "SKIP  $name (no Wine PE builtin dir)"; continue; fi
    miss=""
    while IFS= read -r dll || [ -n "$dll" ]; do
      [ -z "$dll" ] && continue
      if [ -f "$WINE_PE_DIR/$dll" ]; then
        withdll+=(--with-dll "$dll=$WINE_PE_DIR/$dll")
      else
        miss="$dll"
      fi
    done < "$CORPUS/$name.withdll"
    [ -n "$miss" ] && { echo "SKIP  $name ($miss not in $WINE_PE_DIR)"; continue; }
  fi
  if ! "$MINGW" -O1 -w $xcflags "$src" $imp_lib $res_obj -lversion -lole32 -loleaut32 -luser32 -lgdi32 -lcomctl32 -llz32 -o "$TMP/$name.exe" 2>"$TMP/err"; then
    echo "FAIL  $name (PE build: $(head -1 "$TMP/err"))"; continue
  fi
  # Optional per-program arguments: one per line in winecorpus/NAME.args. Passed
  # identically to both Wine and ARET, so command-line handling is exercised.
  pargs=()
  if [ -f "$CORPUS/$name.args" ]; then
    while IFS= read -r line || [ -n "$line" ]; do pargs+=("$line"); done < "$CORPUS/$name.args"
  fi
  # Optional stdin: winecorpus/NAME.in is fed identically to both engines, so the
  # CRT stdin path (getchar -> _filbuf refill, fclose(stdin) on exit) is exercised.
  infile="$CORPUS/$name.in"; [ -f "$infile" ] || infile=/dev/null
  # Optional per-program "no display": winecorpus/NAME.nodisplay unsets DISPLAY for
  # both engines, so an API that needs a display (MessageBox, a modal dialog) takes
  # its deterministic no-display path instead of blocking on a real window. (Wine's
  # MessageBoxA returns -1 immediately with no DISPLAY; a windowed fixture omits the
  # marker and keeps the Xvfb display.) Applied identically to Wine and ARET.
  disp=()
  [ -f "$CORPUS/$name.nodisplay" ] && disp=(env -u DISPLAY)
  # Oracle: real PE under Wine. Run from the temp dir so any files land there.
  oracle="$(cd "$TMP" && "${disp[@]}" wine "$TMP/$name.exe" "${pargs[@]}" <"$infile" 2>/dev/null | norm)"
  # ARET: transpile + run the same PE natively (args after `--`).
  rm -rf "$TMP/out"
  got="$(cd "$TMP" && "${disp[@]}" "$ARET" "$TMP/$name.exe" "${withdll[@]}" --mode transpile --out-dir "$TMP/out" --run -- "${pargs[@]}" <"$infile" 2>"$TMP/aerr" \
        | extract_aret | norm)"
  if [ "$oracle" = "$got" ]; then
    pass=$((pass+1)); echo "  ok    $name"
  elif [ -z "$got" ]; then
    echo "FAIL  $name (no ARET output; $(grep -iE 'abort|unmodelled|unimplemented' "$TMP/aerr" | head -1))"
  else
    echo "DIFF  $name"
    diff <(printf '%s\n' "$oracle") <(printf '%s\n' "$got") | head -8 | sed 's/^/        /'
  fi
done

echo "------------------------------------------"
echo "OS-API (Wine) equivalence: $pass/$total programs"
[ "$pass" -eq "$total" ]
