#!/usr/bin/env bash
# Exhaustive verification of the unsigned magic-division rewrite
# (src/opt/mod.rs: magicu32 / try_magic_udiv). The compiler turns `x / c` into a
# high-multiply + shift; ARET recovers `x / c`. We compile the originals at -O2
# (so the magic idiom is emitted), decompile each with ARET, and compare the two
# over EVERY 32-bit input. This is stronger than an SMT proof for the 32-bit case
# and far faster (Z3 bit-blasts the 64-bit multiply and does not converge).
set -u
ARET="${ARET:-target/release/aret}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Divisors to cover (non-power-of-two constants trigger the magic idiom).
DIVS="3 5 6 7 9 10 11 100 1000"

# Generate the corpus: one unsigned division per divisor. A leading pad keeps
# every divisor off address 0 (which ARET's symbol lookup skips by name).
{
  echo '#include <stdint.h>'
  echo 'unsigned _pad(unsigned x){ return x; }'
  for d in $DIVS; do echo "unsigned div$d(unsigned x){ return x/$d; }"; done
} > "$TMP/src.c"
gcc -O2 -c "$TMP/src.c" -o "$TMP/orig.o" || { echo "corpus build failed"; exit 1; }

# Emit each ARET decompilation under a stable name and collect them. Each emit
# carries its own copy of the C helper preamble (the `__fp32`/`__ix_*` typedefs
# and inlines); concatenating them verbatim would redefine those types (a hard
# "conflicting types" error). So keep the preamble from the *first* emit only and
# append just the function (its forward decl + body, from the first
# `uint64_t aret_divN` line) for the rest.
ARET_C="$TMP/aret.c"
: > "$ARET_C"
echo '#include <stdint.h>' >> "$ARET_C"
RECOVERED=0
FIRST=1
for d in $DIVS; do
  emit="$("$ARET" "$TMP/orig.o" --mode emit --function "div$d" 2>/dev/null \
        | sed -E "s/sub_[0-9a-f]+/aret_div$d/g" | grep -v '#include')" || continue
  if [ "$FIRST" = 1 ]; then
    printf '%s\n' "$emit" >> "$ARET_C"          # full: shared preamble + function
    FIRST=0
  else
    printf '%s\n' "$emit" | awk "/^uint64_t aret_div$d[ (]/{p=1} p" >> "$ARET_C"
  fi
  # Did the rewrite actually fire? (informational)
  if printf '%s\n' "$emit" | grep -q " / "; then
    RECOVERED=$((RECOVERED+1))
  fi
done

# Driver: compare ARET vs original over all 2^32 inputs.
{
  echo '#include <stdint.h>'
  echo '#include <stdio.h>'
  for d in $DIVS; do
    echo "unsigned div$d(unsigned);"
    echo "uint64_t aret_div$d(uint64_t);"
  done
  echo 'int main(void){ uint64_t x=0; do {'
  for d in $DIVS; do
    echo "  if((uint32_t)aret_div$d(x)!=div$d((unsigned)x)){printf(\"div$d mismatch at %llu\\n\",(unsigned long long)x);return 1;}"
  done
  echo '  x++; } while (x < 0x100000000ULL);'
  printf '  printf("ALL 2^32 INPUTS EQUIVALENT\\n"); return 0; }\n'
} > "$TMP/drv.c"

if ! gcc -O2 -w -fno-builtin "$TMP/drv.c" "$ARET_C" "$TMP/orig.o" -o "$TMP/run" 2>"$TMP/err"; then
  echo "FAIL  build: $(head -1 "$TMP/err")"; exit 1
fi
echo "magic-division rewrite fired for $RECOVERED/$(echo $DIVS | wc -w) divisors"
"$TMP/run"
