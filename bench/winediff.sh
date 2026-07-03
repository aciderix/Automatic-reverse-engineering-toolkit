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
trap 'rm -rf "$TMP"' EXIT
MINGW="${MINGW:-i686-w64-mingw32-gcc}"
WINDRES="${WINDRES:-${MINGW%-gcc}-windres}"

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
  name="$(basename "$src" .c)"; total=$((total+1))
  # Optional Windows resource (NAME.rc): compiled with windres and linked in, so
  # tests can embed resources (e.g. a VS_VERSIONINFO block for the version APIs).
  res_obj=""
  if [ -f "$CORPUS/$name.rc" ] && command -v "$WINDRES" >/dev/null 2>&1; then
    "$WINDRES" "$CORPUS/$name.rc" -O coff -o "$TMP/$name.res.o" 2>"$TMP/err" || \
      { echo "FAIL  $name (windres: $(head -1 "$TMP/err"))"; continue; }
    res_obj="$TMP/$name.res.o"
  fi
  # Link the common Win32 libs a guard might reference (version info, OLE/COM,
  # BSTR). Harmless for programs that use none — the imports are demand-loaded.
  if ! "$MINGW" -O1 -w "$src" $res_obj -lversion -lole32 -loleaut32 -o "$TMP/$name.exe" 2>"$TMP/err"; then
    echo "FAIL  $name (PE build: $(head -1 "$TMP/err"))"; continue
  fi
  # Optional per-program arguments: one per line in winecorpus/NAME.args. Passed
  # identically to both Wine and ARET, so command-line handling is exercised.
  pargs=()
  if [ -f "$CORPUS/$name.args" ]; then
    while IFS= read -r line || [ -n "$line" ]; do pargs+=("$line"); done < "$CORPUS/$name.args"
  fi
  # Oracle: real PE under Wine. Run from the temp dir so any files land there.
  oracle="$(cd "$TMP" && wine "$TMP/$name.exe" "${pargs[@]}" 2>/dev/null | norm)"
  # ARET: transpile + run the same PE natively (args after `--`).
  rm -rf "$TMP/out"
  got="$(cd "$TMP" && "$ARET" "$TMP/$name.exe" --mode transpile --out-dir "$TMP/out" --run -- "${pargs[@]}" 2>"$TMP/aerr" \
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
