#!/usr/bin/env bash
# Differential verification (roadmap §8.2 level 2): for each corpus function,
# decompile it with ARET, recompile, and compare against the original compiled
# function on random inputs. Reports per-function PASS/FAIL and a total.
set -u
ARET="${ARET:-target/release/aret}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

gcc -O1 -g -c "$DIR/corpus.c" -o "$TMP/corpus.o" || { echo "corpus build failed"; exit 1; }

# Functions to test (name and argument count).
FUNCS="add1:1 addsub:2 mulshift:1 maxi:2 mini:2 absdiff:2 sign:1 clampu:1 mix:2 sumto:1 countbits:1"

pass=0; total=0
for entry in $FUNCS; do
  name="${entry%%:*}"; argc="${entry##*:}"; total=$((total+1))
  # ARET decompiles the function; rename its single sub_xxxx definition.
  "$ARET" "$TMP/corpus.o" --mode emit --function "$name" 2>/dev/null \
    | sed -E "s/sub_[0-9a-f]+/aret_target/g" > "$TMP/aret.c"
  if ! gcc -O1 -w -c "$TMP/aret.c" -o "$TMP/aret.o" 2>"$TMP/err"; then
    echo "FAIL  $name  (recompile error: $(head -1 "$TMP/err"))"; continue
  fi
  # Generic harness: call both through a 3-int prototype, compare low 32 bits.
  cat > "$TMP/h.c" <<HEOF
#include <stdint.h>
#include <stdio.h>
uint64_t ${name}(uint64_t,uint64_t,uint64_t);
uint64_t aret_target(uint64_t,uint64_t,uint64_t);
static unsigned long long rng=0x2545F4914F6CDD1DULL;
static uint64_t rnd(void){ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; }
int main(void){
  for(int i=0;i<200000;i++){
    /* small, sign-varied inputs so loop-based functions stay bounded */
    uint64_t a=(int)(rnd()%64)-16, b=(int)(rnd()%64)-16, c=(int)(rnd()%64)-16;
    uint32_t o=(uint32_t)${name}(a,b,c);
    uint32_t r=(uint32_t)aret_target(a,b,c);
    if(o!=r){ printf("mismatch on (%lld,%lld,%lld): orig=%u aret=%u\n",(long long)a,(long long)b,(long long)c,o,r); return 1; }
  }
  return 0;
}
HEOF
  if ! gcc -O1 -w "$TMP/h.c" "$TMP/aret.o" "$TMP/corpus.o" -o "$TMP/run" 2>"$TMP/err"; then
    echo "FAIL  $name  (link error: $(head -1 "$TMP/err"))"; continue
  fi
  if "$TMP/run" > "$TMP/out" 2>&1; then
    echo "PASS  $name  (200k random inputs equivalent)"; pass=$((pass+1))
  else
    echo "FAIL  $name  ($(head -1 "$TMP/out"))"
  fi
done
echo "------------------------------------------"
echo "differential equivalence: $pass/$total functions"
