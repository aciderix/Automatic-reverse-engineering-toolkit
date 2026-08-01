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
#
# PARALLEL. Fixtures are independent, so they run `nproc` at a time. Two things make
# that sound rather than merely fast:
#   - each fixture gets its OWN working directory. Several of them create files
#     (temp files, directory trees for findfirst, a DLL that must sit beside its exe)
#     and the serial version ran them all in one shared $TMP with a shared scratch
#     `err`/`aerr`/`out`. Sharing that under -P would produce WRONG verdicts, not just
#     interleaved ones.
#   - output is buffered per fixture and replayed in the original (sorted) order, so
#     the log stays byte-comparable with a serial run — which is exactly how this
#     rewrite was validated.
# A child is a fresh `bash "$0" --one NAME`, not an exported function: it re-reads the
# whole script, so there is no `export -f` environment to keep in sync.
# WINEDIFF_JOBS=1 forces serial execution (for bisecting a flaky fixture).
set -u
ARET="${ARET:-target/release/aret}"
# Absolute path: the run loop cd's into the temp dir (so program-created files
# land there), which would break a relative ARET path.
ARET="$(readlink -f "$ARET" 2>/dev/null || echo "$ARET")"
DIR="$(cd "$(dirname "$0")" && pwd)"
CORPUS="$DIR/winecorpus"
MINGW="${MINGW:-i686-w64-mingw32-gcc}"
WINDRES="${WINDRES:-${MINGW%-gcc}-windres}"
# Wine's PE builtin DLL dir (for `NAME.withdll` fixtures that lift a system DLL
# like comctl32 alongside the app — the same DLL Wine loads as the oracle).
WINE_PE_DIR=""
for d in /usr/lib/i386-linux-gnu/wine/i386-windows /usr/lib/wine/i386-windows \
         /opt/wine-stable/lib/wine/i386-windows; do
  [ -d "$d" ] && WINE_PE_DIR="$d" && break
done

# Program output of `aret --mode transpile --run` is delimited by a marker, each
# line prefixed "  | ".
extract_aret() { awk '/--- program output ---/{f=1;next} f{sub(/^  \| ?/,"");print}'; }
norm() { tr -d '\r'; }   # ignore CRLF-vs-LF line-ending differences

# ---------------------------------------------------------------------------
# One fixture, entirely inside its own working directory $WD. Everything the
# serial version put in the shared $TMP — the exe, the companion DLL, the resource
# object, the import lib, the ARET out-dir, the compiler stderr — lives here, and
# the program itself runs with $WD as cwd so anything it creates stays local.
# Exit status: 0 pass, 1 fail/diff, 2 skip.
# ---------------------------------------------------------------------------
run_one() {
  local name="$1" WD="$2"
  local src="$CORPUS/$name.c"
  mkdir -p "$WD" || return 1
  # Optional Windows resource (NAME.rc): compiled with windres and linked in, so
  # tests can embed resources (e.g. a VS_VERSIONINFO block for the version APIs).
  local res_obj=""
  if [ -f "$CORPUS/$name.rc" ] && command -v "$WINDRES" >/dev/null 2>&1; then
    # -I CORPUS so an .rc can reference sibling files (e.g. a BITMAP "foo.bmp").
    "$WINDRES" -I "$CORPUS" "$CORPUS/$name.rc" -O coff -o "$WD/$name.res.o" 2>"$WD/err" || \
      { echo "FAIL  $name (windres: $(head -1 "$WD/err"))"; return 1; }
    res_obj="$WD/$name.res.o"
  fi
  # Optional per-program compile flags (winecorpus/NAME.cflags, whitespace-separated)
  # — e.g. -mstackrealign to exercise the GCC stack-realignment prologue.
  local xcflags=""; [ -f "$CORPUS/$name.cflags" ] && xcflags="$(cat "$CORPUS/$name.cflags")"
  # Optional per-program import def (winecorpus/NAME.def): built into an import lib
  # with dlltool and linked *first*, so a fixture can force an import that the named
  # system libs would otherwise provide by name — e.g. an import BY ORDINAL (comctl32
  # InitCommonControls @17). Placed right after $src so it wins the symbol.
  local imp_lib=""
  if [ -f "$CORPUS/$name.def" ] && command -v "${MINGW%-gcc}-dlltool" >/dev/null 2>&1; then
    if "${MINGW%-gcc}-dlltool" -d "$CORPUS/$name.def" -l "$WD/$name.imp.a" 2>"$WD/err"; then
      imp_lib="$WD/$name.imp.a"
    else
      echo "FAIL  $name (dlltool: $(head -1 "$WD/err"))"; return 1
    fi
  fi
  # Optional companion DLL (winecorpus/NAME.dll.c): built as a real PE DLL the app
  # imports from; ARET lifts it too via `--with-dll` (DLL lifting, doc 80 §1.2) so
  # the app's imports of its exports dispatch to lifted code, not an HLE shim. The
  # DLL sits beside the app, so Wine (the oracle) loads it normally.
  local withdll=() dllname
  if [ -f "$CORPUS/$name.dll.c" ]; then
    dllname="${name}dll.dll"
    if ! "$MINGW" -O1 -w -shared "$CORPUS/$name.dll.c" -o "$WD/$dllname" \
         -Wl,--out-implib,"$WD/$name.dllimp.a" 2>"$WD/err"; then
      echo "FAIL  $name (DLL build: $(head -1 "$WD/err"))"; return 1
    fi
    imp_lib="$imp_lib $WD/$name.dllimp.a"       # link the app against the DLL
    withdll=(--with-dll "$dllname=$WD/$dllname") # and lift the DLL under ARET
  fi
  # Optional system-DLL lifting (winecorpus/NAME.withdll): one DLL name per line
  # (e.g. `comctl32.dll`) that ARET lifts from Wine's own PE builtins — the app
  # links against them normally and Wine (oracle) loads the same DLLs. Skips the
  # fixture if the builtin dir or a named DLL is missing (like a toolchain gate).
  local miss dll
  if [ -f "$CORPUS/$name.withdll" ]; then
    if [ -z "$WINE_PE_DIR" ]; then echo "SKIP  $name (no Wine PE builtin dir)"; return 2; fi
    miss=""
    while IFS= read -r dll || [ -n "$dll" ]; do
      [ -z "$dll" ] && continue
      if [ -f "$WINE_PE_DIR/$dll" ]; then
        withdll+=(--with-dll "$dll=$WINE_PE_DIR/$dll")
      else
        miss="$dll"
      fi
    done < "$CORPUS/$name.withdll"
    [ -n "$miss" ] && { echo "SKIP  $name ($miss not in $WINE_PE_DIR)"; return 2; }
  fi
  # Link the common Win32 libs a guard might reference (version info, OLE/COM,
  # BSTR, common controls). Harmless for programs that use none — the imports are
  # demand-loaded.
  if ! "$MINGW" -O1 -w $xcflags "$src" $imp_lib $res_obj -lversion -lole32 -loleaut32 -luser32 -lgdi32 -lcomctl32 -lwinspool -llz32 -lshlwapi -o "$WD/$name.exe" 2>"$WD/err"; then
    echo "FAIL  $name (PE build: $(head -1 "$WD/err"))"; return 1
  fi
  # Optional per-program arguments: one per line in winecorpus/NAME.args. Passed
  # identically to both Wine and ARET, so command-line handling is exercised.
  local pargs=() line
  if [ -f "$CORPUS/$name.args" ]; then
    while IFS= read -r line || [ -n "$line" ]; do pargs+=("$line"); done < "$CORPUS/$name.args"
  fi
  # Optional stdin: winecorpus/NAME.in is fed identically to both engines, so the
  # CRT stdin path (getchar -> _filbuf refill, fclose(stdin) on exit) is exercised.
  local infile="$CORPUS/$name.in"; [ -f "$infile" ] || infile=/dev/null
  # Optional per-program "no display": winecorpus/NAME.nodisplay unsets DISPLAY for
  # both engines, so an API that needs a display (MessageBox, a modal dialog) takes
  # its deterministic no-display path instead of blocking on a real window. (Wine's
  # MessageBoxA returns -1 immediately with no DISPLAY; a windowed fixture omits the
  # marker and keeps the Xvfb display.) Applied identically to Wine and ARET.
  local disp=()
  local xpid="" i
  if [ -f "$CORPUS/$name.nodisplay" ]; then
    disp=(env -u DISPLAY)
  elif command -v Xvfb >/dev/null 2>&1; then
    # A PRIVATE X server per fixture. A single shared one is not safe under -P:
    # measured, `win_timechar` (a WS_POPUP created at 100,50 then converted with
    # ClientToScreen) stopped landing where it was asked to when several Wine
    # processes shared a display — and it was the ORACLE side that moved, so the
    # comparison silently became meaningless rather than merely flaky. `-displayfd`
    # lets Xvfb pick a free display number and tell us, so children never collide.
    Xvfb -displayfd 9 -screen 0 1280x1024x24 9>"$WD/dispnum" >/dev/null 2>&1 &
    xpid=$!
    for i in $(seq 1 60); do [ -s "$WD/dispnum" ] && break; sleep 0.05; done
    if [ -s "$WD/dispnum" ]; then
      export DISPLAY=":$(cat "$WD/dispnum")"
    else
      kill "$xpid" 2>/dev/null; xpid=""
    fi
  fi
  # Oracle: real PE under Wine. Run from the fixture dir so any files land there.
  local oracle got
  oracle="$(cd "$WD" && "${disp[@]}" wine "$WD/$name.exe" "${pargs[@]}" <"$infile" 2>/dev/null | norm)"
  # ARET: transpile + run the same PE natively (args after `--`).
  rm -rf "$WD/out"
  got="$(cd "$WD" && "${disp[@]}" "$ARET" "$WD/$name.exe" "${withdll[@]}" --mode transpile --out-dir "$WD/out" --run -- "${pargs[@]}" <"$infile" 2>"$WD/aerr" \
        | extract_aret | norm)"
  [ -n "$xpid" ] && kill "$xpid" 2>/dev/null
  if [ "$oracle" = "$got" ]; then
    echo "  ok    $name"; return 0
  elif [ -z "$got" ]; then
    echo "FAIL  $name (no ARET output; $(grep -iE 'abort|unmodelled|unimplemented' "$WD/aerr" | head -1))"; return 1
  else
    # One echo, so a multi-line report cannot interleave with another fixture's.
    echo "DIFF  $name
$(diff <(printf '%s\n' "$oracle") <(printf '%s\n' "$got") | head -8 | sed 's/^/        /')"
    return 1
  fi
}

# ---------------------------------------------------------------------------
# Child mode: one fixture, into its own log. Setup (Xvfb, wineprefix) was already
# done by the parent and reaches us through the environment.
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--one" ]; then
  name="$2"
  TMP="$ARET_WD_TMP"
  run_one "$name" "$TMP/w/$name" > "$TMP/log/$name.out" 2>&1
  echo "$?" > "$TMP/log/$name.rc"
  exit 0
fi

# ---------------------------------------------------------------------------
# Parent: toolchain gates, shared setup, dispatch, ordered report.
# ---------------------------------------------------------------------------
if ! command -v "$MINGW" >/dev/null 2>&1; then
  echo "SKIP  ($MINGW unavailable; install mingw-w64)"; exit 0
fi
if ! command -v wine >/dev/null 2>&1; then
  echo "SKIP  (wine unavailable; install wine)"; exit 0
fi

TMP="$(mktemp -d)"
XVFB_PID=""
cleanup() { [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
# Headless X. A display must exist while `wineboot --init` runs below: Wine configures
# its X11 driver then, and a prefix initialised with no display places windows
# differently afterwards (measured on win_timechar). Each fixture then gets its OWN
# server on top of this one (see run_one), because window placement also stops being
# deterministic when concurrent Wine processes share a display.
if command -v Xvfb >/dev/null 2>&1; then
  Xvfb :99 -screen 0 1280x1024x24 >/dev/null 2>&1 &
  XVFB_PID=$!
  export DISPLAY=:99
  sleep 0.4
fi
# Isolated, quiet Wine prefix; initialise it once up front — before any child runs,
# so concurrent wine processes attach to a prefix that is already built.
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEPREFIX="$TMP/wineprefix"
# Fixed timezone + locale so date/time/codepage conversions are deterministic.
export TZ=UTC
export LC_ALL=C
wine wineboot --init >/dev/null 2>&1 || true

# Optional single-fixture selection: `winediff.sh NAME` runs just that one.
sel="${1:-}"
names=()
for src in "$CORPUS"/*.c; do
  # Companion DLL sources (NAME.dll.c) are built by their app fixture, not run
  # standalone — skip them in the main loop.
  case "$src" in *.dll.c) continue;; esac
  n="$(basename "$src" .c)"
  [ -n "$sel" ] && [ "$n" != "$sel" ] && continue
  names+=("$n")
done
if [ "${#names[@]}" -eq 0 ]; then
  echo "SKIP  (no fixture matches '${sel}')"; exit 0
fi

mkdir -p "$TMP/log" "$TMP/w"
export ARET_WD_TMP="$TMP"
jobs="${WINEDIFF_JOBS:-$(nproc 2>/dev/null || echo 4)}"
# Window-creating fixtures run SERIALLY, the rest in parallel. Measured, not assumed:
# with everything parallel, `user32_listbox` and `user32_isdlgmsg` intermittently came
# back degenerate — and on the ORACLE side, Wine reporting an empty listbox and no
# focus. A private Xvfb per fixture (see run_one) is necessary but NOT sufficient;
# concurrent Wine GUI clients on one prefix still interfere. Each one passes 3/3 alone.
# 43 of 194 fixtures create a window, so the parallel win is kept for the other 151
# while the flaky class is removed rather than tolerated — a gate that goes red at
# random teaches you to ignore red, which is worse than a slow gate.
# A `winecorpus/NAME.serial` marker forces a fixture into the serial set even when it
# creates no window. Two were caught that way (comctl_loadbitmap, console_cp): under
# load it was the ORACLE that produced no output at all, not ARET that answered wrong
# — the same signature as the "104 FAILs" that were really a full /tmp. Each passes
# alone. An explicit marker file is used rather than growing the regex, because the
# reason differs per fixture (comctl32 image-list init, console attach) and a regex
# would silently claim they are all the same kind of thing.
gui=(); cli=()
for n in "${names[@]}"; do
  if [ -f "$CORPUS/$n.serial" ]; then
    gui+=("$n")
  elif grep -qE 'CreateWindow|DialogBox|CreateDialog' "$CORPUS/$n.c" 2>/dev/null; then
    gui+=("$n")
  else
    cli+=("$n")
  fi
done
[ "${#cli[@]}" -gt 0 ] && printf '%s\0' "${cli[@]}" | xargs -0 -P "$jobs" -I{} bash "$0" --one {}
[ "${#gui[@]}" -gt 0 ] && printf '%s\0' "${gui[@]}" | xargs -0 -P 1 -I{} bash "$0" --one {}

# Replay in the original order: the log is then byte-identical to a serial run.
pass=0; total=0
for n in "${names[@]}"; do
  total=$((total+1))
  [ -f "$TMP/log/$n.out" ] && cat "$TMP/log/$n.out"
  [ "$(cat "$TMP/log/$n.rc" 2>/dev/null || echo 1)" = "0" ] && pass=$((pass+1))
done

echo "------------------------------------------"
echo "OS-API (Wine) equivalence: $pass/$total programs"
[ "$pass" -eq "$total" ]
