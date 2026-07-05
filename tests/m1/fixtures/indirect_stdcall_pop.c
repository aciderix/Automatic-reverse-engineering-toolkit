/* Minimal reproduction of the BusyBox `cksum` crash: an indirect FAST_FUNC
   (regparm(3)+stdcall, `ret 4` — callee pops its stack arg) called in a loop,
   WITH the table pointer spilled to an esp-relative slot BEFORE the loop and
   reloaded from it INSIDE the loop (BusyBox's crc-table at [esp+0x28]).

   The compiler emits its own `sub esp,4`/dummy `push` after the call, assuming the
   callee popped 4 bytes. If the transpiler models that compensation but NOT the
   callee's `ret 4` pop, esp drifts -4 per iteration; the reload then reads the
   table from the wrong (drifted) slot -> garbage/crash. The fix models the pop at
   internal call sites (ir::build::callee_pop_adjust + __aret_callee_pop).

   Build (i686 mingw, -fomit-frame-pointer so locals are esp-relative like BusyBox):
     i686-w64-mingw32-gcc -O2 -fomit-frame-pointer -o indirect_stdcall_pop.exe \
       indirect_stdcall_pop.c
   Native output: c=226. Pre-fix transpile: SEGFAULT. Post-fix transpile: c=226. */
#include <stdio.h>
static unsigned tblA[1] = {42};
static unsigned tblB[1] = {7};
typedef unsigned (__attribute__((regparm(3), stdcall)) *cbfn)(unsigned,const void*,unsigned,const unsigned*);
static unsigned __attribute__((regparm(3), stdcall, noinline))
cbA(unsigned crc, const void *buf, unsigned len, const unsigned *table){ (void)buf;(void)len; return table[0]+crc; }
static unsigned __attribute__((regparm(3), stdcall, noinline))
cbB(unsigned crc, const void *buf, unsigned len, const unsigned *table){ (void)buf;(void)len; return table[0]*crc; }
static cbfn slot;
static unsigned driver(const void *b, unsigned n) __attribute__((noinline));
static unsigned driver(const void *b, unsigned n){
    /* force the table pointer to a stack slot (address taken -> spilled, esp-rel
       with -fomit-frame-pointer), written once here, reloaded each iteration */
    const unsigned *tp = tblA;
    const unsigned **tpp = &tp;
    __asm__ volatile("" : : "r"(tpp) : "memory"); /* make &tp escape -> tp lives in memory */
    unsigned c = 100;
    for (unsigned i=0;i<n;i++)
        c = slot(c, b, 1, *tpp);   /* reload tp from its stack slot every iteration */
    return c;
}
int main(int argc, char **argv){
    slot = (argc > 99) ? cbB : cbA;
    char b[4]={0};
    printf("c=%u\n", driver(b, 3));   /* cbA over tblA=42: ((100+42)+42)+42 = 226 */
    return 0;
}
