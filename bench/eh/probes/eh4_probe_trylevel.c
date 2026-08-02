/* I4 probe C: is `trylevel` XOR-encoded with *cookie, like `scopetable` is?
 * Made safe in BOTH directions: cookie = 4, eight levels, so the plain answer (2)
 * and the encoded answer (2 ^ 4 = 6) are both valid indices with their own filter.
 * Whichever prints IS the answer -- no crash either way, so neither outcome is mute. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef struct { DWORD prev, handler, scopetable, trylevel, _ebp, xpointers; } REG4;
typedef struct { int enclosing; DWORD filter, handler; } REC;
typedef struct { int gs_off, gs_xor, eh_off, eh_xor; REC r[8]; } TBL;
int __cdecl _except_handler4_common(ULONG*, void(*)(void), EXCEPTION_RECORD*, void*, CONTEXT*, void*);
static void __cdecl check_cookie(void){ printf("  CHECK-COOKIE\n"); fflush(stdout); }
static int lvl_seen = -99;
#define F(n) static int __cdecl f##n(void){ printf("  filter %d\n", n); fflush(stdout); lvl_seen=n; return 0; }
F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7)
static ULONG cookie = 4;
static TBL t;
static void call_it(const char *tag, DWORD stored_level){
    REG4 f; EXCEPTION_RECORD rec; CONTEXT ctx; void *disp=0;
    memset(&rec,0,sizeof rec); memset(&ctx,0,sizeof ctx);
    rec.ExceptionCode=0xC0000005; rec.ExceptionFlags=0;
    f.prev=0xFFFFFFFFu; f.handler=0;
    f.scopetable = (DWORD)(ULONG_PTR)&t ^ cookie;      /* PROVEN encoding */
    f.trylevel = stored_level; f._ebp=0; f.xpointers=0;
    lvl_seen = -99;
    printf("%s: stored trylevel = %ld\n", tag, (long)(int)stored_level); fflush(stdout);
    int r = _except_handler4_common(&cookie, check_cookie, &rec, &f, &ctx, &disp);
    printf("  -> returned %d, first filter seen = %d\n", r, lvl_seen); fflush(stdout);
}
int main(void){
    DWORD fs[8]={(DWORD)(ULONG_PTR)f0,(DWORD)(ULONG_PTR)f1,(DWORD)(ULONG_PTR)f2,(DWORD)(ULONG_PTR)f3,
                 (DWORD)(ULONG_PTR)f4,(DWORD)(ULONG_PTR)f5,(DWORD)(ULONG_PTR)f6,(DWORD)(ULONG_PTR)f7};
    t.gs_off=-2; t.gs_xor=0; t.eh_off=-2; t.eh_xor=0;
    /* Every level terminates immediately, so exactly ONE filter runs per call and
     * the answer is unambiguous. */
    for (int i=0;i<8;i++){ t.r[i].enclosing=-2; t.r[i].filter=fs[i]; t.r[i].handler=0; }
    printf("cookie = %lu\n", (unsigned long)cookie);
    call_it("stored 2 (plain if 2 runs, encoded if 6 runs)", 2);
    call_it("stored 6 (plain if 6 runs, encoded if 2 runs)", 6);
    return 0;
}
