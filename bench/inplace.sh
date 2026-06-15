#!/usr/bin/env bash
# In-place differential verification: validates functions whose decompilation
# reads the binary's data by *absolute address* (globals, .rodata jump/value
# tables) — which the standalone harness cannot run (those addresses are
# unmapped). Here we map the original binary's PT_LOAD segments at their virtual
# addresses, so the decompiled code's `*(T*)(globalVA)` reads the real bytes,
# and compare to the source-built reference (identical data values).
set -u
ARET="${ARET:-target/release/aret}"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Non-PIE exe (functions + tables at fixed VAs) — the decompilation/data source.
cat > "$TMP/keep.c" <<'C'
int lut(int), swv(int);
__attribute__((used)) void* keep[] = { (void*)lut, (void*)swv };
int main(void){ return 0; }
C
gcc -no-pie -O2 "$DIR/corpus.c" "$TMP/keep.c" -o "$TMP/exe" 2>/dev/null \
  || { echo "exe build failed"; exit 1; }
# Source reference object (linked into the PIE harness).
gcc -O2 -w -c "$DIR/corpus.c" -o "$TMP/ref.o" 2>/dev/null

FUNCS="lut:i swv:i"
pass=0; total=0
for entry in $FUNCS; do
  name="${entry%%:*}"; total=$((total+1))
  "$ARET" "$TMP/exe" --mode emit --function "$name" 2>/dev/null \
    | sed -E "s/sub_[0-9a-f]+/aret_target/g" > "$TMP/aret.c"
  if grep -q "decompilation INCOMPLETE" "$TMP/aret.c"; then
    echo "INCOMPLETE  $name"; continue
  fi
  if ! gcc -O1 -w -fno-builtin -c "$TMP/aret.c" -o "$TMP/aret.o" 2>"$TMP/e"; then
    echo "FAIL  $name (recompile: $(head -1 "$TMP/e"))"; continue
  fi
  cat > "$TMP/h.c" <<HEOF
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
int ${name}(int);
uint64_t aret_target(uint64_t,uint64_t,uint64_t);
/* Map the original binary's loadable segments at their virtual addresses so the
   decompiled function's absolute-address reads resolve to the real data. */
static void map_exe(const char* p){
  int fd=open(p,O_RDONLY); struct stat st; fstat(fd,&st);
  unsigned char* f=mmap(0,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);
  Elf64_Ehdr* eh=(void*)f; Elf64_Phdr* ph=(void*)(f+eh->e_phoff);
  for(int i=0;i<eh->e_phnum;i++){
    if(ph[i].p_type!=PT_LOAD) continue;
    uint64_t va=ph[i].p_vaddr, fsz=ph[i].p_filesz, msz=ph[i].p_memsz;
    uint64_t b=va&~0xfffULL, e=(va+msz+0xfffULL)&~0xfffULL;
    if(mmap((void*)b,e-b,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==MAP_FAILED){
      perror("mmap"); return;
    }
    memcpy((void*)va, f+ph[i].p_offset, fsz);
  }
}
static unsigned long long rng=0x2545F4914F6CDD1DULL;
static uint64_t rnd(void){ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; }
int main(void){
  map_exe("${TMP}/exe");
  for(int it=0; it<200000; it++){
    int x=(int)(rnd()%64)-8;
    uint32_t o=(uint32_t)${name}(x);
    uint32_t r=(uint32_t)aret_target((uint64_t)(int64_t)x,0,0);
    if(o!=r){ printf("mismatch x=%d orig=%u aret=%u\n",x,o,r); return 1; }
  }
  return 0;
}
HEOF
  if ! gcc -O1 -w -fno-builtin "$TMP/h.c" "$TMP/aret.o" "$TMP/ref.o" -o "$TMP/run" 2>"$TMP/e"; then
    echo "FAIL  $name (link: $(head -1 "$TMP/e"))"; continue
  fi
  if "$TMP/run" >"$TMP/o" 2>&1; then
    echo "PASS  $name (in-place, 200k inputs)"; pass=$((pass+1))
  else
    echo "FAIL  $name ($(head -1 "$TMP/o"))"
  fi
done
echo "------------------------------------------"
echo "in-place equivalence: $pass/$total functions"
