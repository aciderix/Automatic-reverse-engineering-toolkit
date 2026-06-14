#!/usr/bin/env bash
# Differential verification (roadmap §8.2 level 2): decompile each corpus
# function with ARET, recompile, and compare to the original on random inputs.
# Modes: i = int args; p = (int* a, int n, int x); s = (char* s).
set -u
ARET="${ARET:-target/release/aret}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

gcc -O1 -g -c "$DIR/corpus.c" -o "$TMP/corpus.o" || { echo "corpus build failed"; exit 1; }

FUNCS="add1:i addsub:i mulshift:i maxi:i mini:i absdiff:i sign:i clampu:i mix:i sumto:i countbits:i \
       arraysum:p arraymax:p third:p counteq:p strlen_c:s udiv:i umod:i sdiv:i smod:i widemul:i"

pass=0; total=0
for entry in $FUNCS; do
  name="${entry%%:*}"; mode="${entry##*:}"; total=$((total+1))
  "$ARET" "$TMP/corpus.o" --mode emit --function "$name" 2>/dev/null \
    | sed -E "s/sub_[0-9a-f]+/aret_target/g" > "$TMP/aret.c"
  if ! gcc -O1 -w -c "$TMP/aret.c" -o "$TMP/aret.o" 2>"$TMP/err"; then
    echo "FAIL  $name  (recompile: $(head -1 "$TMP/err"))"; continue
  fi
  case "$mode" in
    i) DECL='uint64_t a0=(int)(rnd()%64)-16,a1=(int)(rnd()%64)-16,a2=(int)(rnd()%64)-16;' ;;
    p) DECL='uint64_t a0=(uint64_t)(uintptr_t)ibuf,a1=(uint64_t)(rnd()%65),a2=(uint64_t)((int)(rnd()%32)-8);' ;;
    s) DECL='uint64_t a0=(uint64_t)(uintptr_t)cbuf,a1=0,a2=0;' ;;
  esac
  cat > "$TMP/h.c" <<HEOF
#include <stdint.h>
#include <stdio.h>
uint64_t ${name}(uint64_t,uint64_t,uint64_t);
uint64_t aret_target(uint64_t,uint64_t,uint64_t);
static unsigned long long rng=0x2545F4914F6CDD1DULL;
static uint64_t rnd(void){ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; }
int main(void){
  for(int it=0; it<50000; it++){
    int ibuf[65]; char cbuf[65];
    for(int i=0;i<65;i++){ ibuf[i]=(int)(rnd()%200)-100; cbuf[i]=(char)(1+rnd()%90); }
    cbuf[rnd()%65]=0;
    ${DECL}
    uint32_t o=(uint32_t)${name}(a0,a1,a2);
    uint32_t r=(uint32_t)aret_target(a0,a1,a2);
    if(o!=r){ printf("mismatch: orig=%u aret=%u\n",o,r); return 1; }
  }
  return 0;
}
HEOF
  if ! gcc -O1 -w "$TMP/h.c" "$TMP/aret.o" "$TMP/corpus.o" -o "$TMP/run" 2>"$TMP/err"; then
    echo "FAIL  $name  (link: $(head -1 "$TMP/err"))"; continue
  fi
  if "$TMP/run" >"$TMP/out" 2>&1; then
    echo "PASS  $name  ($mode, 50k random inputs equivalent)"; pass=$((pass+1))
  else
    echo "FAIL  $name  ($(head -1 "$TMP/out"))"
  fi
done
echo "------------------------------------------"
echo "differential equivalence: $pass/$total functions"
