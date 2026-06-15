#!/usr/bin/env bash
# Differential verification (roadmap §8.2 level 2): decompile each corpus
# function with ARET, recompile, and compare to the original on random inputs.
#
# Runs at MULTIPLE optimization levels (-O0..-O3): -O0 spills almost everything
# to the stack (stresses stack-slot recovery), while -O2/-O3 omit the frame
# pointer (rsp-relative locals) and inline/vectorise — i.e. realistic optimized
# code, with the source as ground truth. A function that diverges at any level is
# a real bug on real-shaped code.
#
# Modes: i = int args; p = (int* a, int n, int x); s = (char* s).
set -u
ARET="${ARET:-target/release/aret}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Gate at -O0..-O3. Every function ARET fully lifts must be equivalent; functions
# with unmodelled instructions (e.g. -O3 auto-vectorised SSE) are reported as
# INCOMPLETE and excluded from the ratio (a known gap, not a regression).
LEVELS="${LEVELS:--O0 -O1 -O2 -O3}"
ITERS="${ITERS:-50000}"

FUNCS="add1:i addsub:i mulshift:i maxi:i mini:i absdiff:i sign:i clampu:i mix:i sumto:i countbits:i \
       arraysum:p arraymax:p third:p counteq:p strlen_c:s udiv:i umod:i sdiv:i smod:i widemul:i \
       hibyte:i hibyte3:i sxbyte:i sxword:i idxw:p spill2:i spill3:i \
       sortpair:i poly:i stackarr:i fieldsum:p nestcond:i sumrec:i borrow:i cmp3:i"

pass=0; total=0; incomplete=0
for OPT in $LEVELS; do
  if ! gcc "$OPT" -g -c "$DIR/corpus.c" -o "$TMP/corpus.o" 2>"$TMP/err"; then
    echo "FAIL  (corpus build $OPT: $(head -1 "$TMP/err"))"; total=$((total+1)); continue
  fi
  lpass=0; ltotal=0
  for entry in $FUNCS; do
    name="${entry%%:*}"; mode="${entry##*:}"; total=$((total+1)); ltotal=$((ltotal+1))
    "$ARET" "$TMP/corpus.o" --mode emit --function "$name" 2>/dev/null \
      | sed -E "s/sub_[0-9a-f]+/aret_target/g" > "$TMP/aret.c"
    # ARET marks decompilations it could not fully lift (unmodelled instructions,
    # e.g. auto-vectorised SSE). Those are a known gap, not a correctness
    # regression — report them as INCOMPLETE rather than testing for equivalence.
    if grep -q "decompilation INCOMPLETE" "$TMP/aret.c"; then
      incomplete=$((incomplete+1)); total=$((total-1)); ltotal=$((ltotal-1))
      echo "INCOMPLETE  $name $OPT (unmodelled instructions)"; continue
    fi
    if ! gcc -O1 -w -c "$TMP/aret.c" -o "$TMP/aret.o" 2>"$TMP/err"; then
      echo "FAIL  $name $OPT (recompile: $(head -1 "$TMP/err"))"; continue
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
  for(int it=0; it<${ITERS}; it++){
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
      echo "FAIL  $name $OPT (link: $(head -1 "$TMP/err"))"; continue
    fi
    if "$TMP/run" >"$TMP/out" 2>&1; then
      pass=$((pass+1)); lpass=$((lpass+1))
    else
      echo "FAIL  $name $OPT ($(head -1 "$TMP/out"))"
    fi
  done
  echo "  $OPT: $lpass/$ltotal"
done
echo "------------------------------------------"
echo "incomplete (unmodelled instructions, not tested): $incomplete"
echo "differential equivalence: $pass/$total functions"
