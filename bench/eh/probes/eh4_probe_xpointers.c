/* I4 probe D: WHERE does _except_handler4_common publish EXCEPTION_POINTERS, and
 * what ebp does it hand the filter? v3 filters read [ebp-0x14]; the v4 frame also
 * has an `xpointers` field at +20. The filter prints both candidates, plus its own
 * incoming ebp, so the answer is read rather than assumed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef struct { DWORD prev, handler, scopetable, trylevel, _ebp, xpointers; } REG4;
typedef struct { int enclosing; DWORD filter, handler; } REC;
typedef struct { int gs_off, gs_xor, eh_off, eh_xor; REC r[4]; } TBL;
int __cdecl _except_handler4_common(ULONG*, void(*)(void), EXCEPTION_RECORD*, void*, CONTEXT*, void*);
static void __cdecl check_cookie(void){ printf("  CHECK-COOKIE\n"); fflush(stdout); }
static REG4 g_f;
static EXCEPTION_RECORD g_rec;
static int __cdecl filt(void){
    DWORD ebp;
    __asm__ __volatile__("movl %%ebp,%0" : "=r"(ebp));
    /* our own prologue pushed ebp, so the INCOMING ebp is at [ebp] */
    DWORD in_ebp = *(DWORD *)(ULONG_PTR)ebp;
    DWORD framep = (DWORD)(ULONG_PTR)&g_f;
    DWORD at_m4  = *(DWORD *)(ULONG_PTR)(framep - 4);
    printf("  filter: incoming ebp = frame%+d\n", (int)(in_ebp - framep));
    printf("  filter: [frame-4]   = %s\n", at_m4 == 0xAAAAAAAAu ? "untouched" : "WRITTEN");
    if (at_m4 != 0xAAAAAAAAu) {
        DWORD *xp = (DWORD *)(ULONG_PTR)at_m4;
        printf("  filter: [frame-4]->rec is our record = %d\n", xp[0] == (DWORD)(ULONG_PTR)&g_rec);
    }
    printf("  filter: frame->xpointers = %s\n",
           g_f.xpointers == 0xAAAAAAAAu ? "untouched" :
           (g_f.xpointers && ((DWORD*)(ULONG_PTR)g_f.xpointers)[0] == (DWORD)(ULONG_PTR)&g_rec
            ? "WRITTEN (points at our record)" : "WRITTEN (other)"));
    fflush(stdout);
    return 0;
}
static ULONG cookie = 0;
static TBL t;
static DWORD guard[4];
int main(void){
    t.gs_off=-2; t.gs_xor=0; t.eh_off=-2; t.eh_xor=0;
    for (int i=0;i<4;i++){ t.r[i].enclosing=-2; t.r[i].filter=(DWORD)(ULONG_PTR)filt; t.r[i].handler=0; }
    CONTEXT ctx; void *disp=0;
    memset(&ctx,0,sizeof ctx); memset(&g_rec,0,sizeof g_rec);
    g_rec.ExceptionCode=0xC0000005;
    /* poison the word BELOW the frame so "was it written" is observable */
    memset(guard, 0xAA, sizeof guard);
    g_f.prev=0xFFFFFFFFu; g_f.handler=0; g_f.scopetable=(DWORD)(ULONG_PTR)&t ^ cookie;
    g_f.trylevel=0; g_f._ebp=0; g_f.xpointers=0xAAAAAAAAu;
    *(DWORD *)((ULONG_PTR)&g_f - 4) = 0xAAAAAAAAu;
    printf("frame at %p, calling\n", (void*)&g_f); fflush(stdout);
    int r = _except_handler4_common(&cookie, check_cookie, &g_rec, &g_f, &ctx, &disp);
    printf("-> returned %d\n", r);
    return 0;
}
