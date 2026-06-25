#!/usr/bin/env bash
# Differential verification of the *transpile* pipeline (Pipeline B / the real UBT
# product), complementing difftest.sh which only exercises `--mode emit`
# (decompilation / Pipeline A).
#
# Why a second harness: in decompile mode an argument is a sign-extended uint64
# parameter; in transpile mode it is a zero-extended uint32 memory load on the
# shared stack. Those differ — and the difference hid a real silent wrong result
# (the width-aware sign-flag bug: a signed `cmp` of a memory operand took the
# wrong branch). This harness builds the whole corpus into one program, transpiles
# it, runs it, and checks the result against the native 32-bit build.
#
# Ground truth: the SAME source compiled `-m32` (matching the PE's 32-bit ABI: 4-byte
# long/pointer, x87 floats). Inputs are kept in the corpus's overflow-safe range so
# the source has no UB and the native run is deterministic, while still including
# negatives — enough to exercise memory-operand sign handling.
set -u
ARET="${ARET:-target/release/aret}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
LEVELS="${LEVELS:--O0 -O1 -O2 -O3}"
MINGW="${MINGW:-i686-w64-mingw32-gcc}"

# Toolchain probe — skip (not fail) when the 32-bit / Windows cross toolchain is
# unavailable, like the Rust tests do.
if ! echo 'int main(void){return 0;}' | gcc -m32 -x c - -o "$TMP/t" 2>/dev/null; then
  echo "SKIP  (gcc -m32 unavailable; install gcc-multilib)"; exit 0
fi
if ! command -v "$MINGW" >/dev/null 2>&1; then
  echo "SKIP  ($MINGW unavailable; install mingw-w64)"; exit 0
fi

python3 "$DIR/gen_transpile_driver.py" "$DIR/corpus.c" > "$TMP/driver.c" 2>"$TMP/gen.log" \
  || { echo "FAIL  (driver generation: $(cat "$TMP/gen.log"))"; exit 1; }
nfuncs="$(sed -n 's/.*generated driver: \([0-9]*\).*/\1/p' "$TMP/gen.log")"

# Native 32-bit ground truth.
if ! gcc -m32 -O1 -w "$TMP/driver.c" -o "$TMP/ref" 2>"$TMP/err"; then
  echo "FAIL  (native -m32 build: $(head -1 "$TMP/err"))"; exit 1
fi
REF="$("$TMP/ref")"

pass=0; total=0
for OPT in $LEVELS; do
  total=$((total+1))
  if ! "$MINGW" "$OPT" -w "$TMP/driver.c" -o "$TMP/app.exe" 2>"$TMP/err"; then
    echo "FAIL  $OPT (PE build: $(head -1 "$TMP/err"))"; continue
  fi
  rm -rf "$TMP/out"
  got="$("$ARET" "$TMP/app.exe" --mode transpile --out-dir "$TMP/out" --run 2>"$TMP/aerr" \
        | grep -oE 'H=[0-9a-f]+' | head -1)"
  if [ "$got" = "$REF" ]; then
    pass=$((pass+1)); echo "  $OPT: ok ($got)"
  elif [ -z "$got" ]; then
    echo "FAIL  $OPT (no output; $(grep -iE 'abort|unmodelled' "$TMP/aerr" | head -1))"
  else
    echo "FAIL  $OPT (transpiled $got != native $REF)"
  fi
done

echo "------------------------------------------"
echo "transpile-pipeline equivalence: $pass/$total opt-levels ($nfuncs functions, ref $REF)"
[ "$pass" -eq "$total" ]
