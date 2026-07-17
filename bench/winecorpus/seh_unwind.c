/* Structured Exception Handling — local unwind via RtlUnwind.
 *
 * RtlUnwind is the primitive __except_handler3 (through __global_unwind2) uses to run
 * the cleanup (__finally) handlers of every frame between the raise point and the
 * frame that catches. On i386 it IGNORES its TargetIp argument: it walks fs:[0] from
 * the head up to — but not including — the TargetFrame, calls each intervening handler
 * with the EH_UNWINDING(0x2) flag set in the exception record, pops each frame off the
 * chain, then RETURNS NORMALLY, leaving fs:[0] at the TargetFrame. (The non-local jump
 * to the __except block is done by the caller afterwards, not by RtlUnwind.)
 *
 * mingw i686 has no __try/__except, so the two SEH frames are installed by hand and we
 * call RtlUnwind directly — exactly what MSVC's __global_unwind2 does. We unwind the
 * inner frame up to the outer and observe: the inner handler was invoked once WITH the
 * unwinding flag, the outer (the target) was NOT, and fs:[0] now points at the outer.
 * The observed pointers are stashed through globals so the result is stable under the
 * optimizer (RtlUnwind restores nonvolatile registers, which otherwise lets GCC cache
 * a stale copy of the frame pointer). Expected (Wine and ARET): inner=1 flags=0x2
 * fs0_target=1. */
#include <windows.h>
#include <stdio.h>

typedef struct EReg { struct EReg *next; void *handler; } EReg;

static int inner_called;
static unsigned inner_flags;
static void *g_outer, *g_fs0;   /* defeat register caching across RtlUnwind */

static EXCEPTION_DISPOSITION __cdecl h_inner(EXCEPTION_RECORD *r, void *fr, CONTEXT *c, void *d) {
    (void)fr; (void)c; (void)d;
    inner_called++; inner_flags = r->ExceptionFlags;
    return ExceptionContinueSearch;
}
static EXCEPTION_DISPOSITION __cdecl h_outer(EXCEPTION_RECORD *r, void *fr, CONTEXT *c, void *d) {
    (void)r; (void)fr; (void)c; (void)d;
    return ExceptionContinueSearch;      /* the target frame — must NOT be called */
}
static void *get_fs0(void) { void *p; __asm__ volatile("movl %%fs:0, %0" : "=r"(p)); return p; }
static void  set_fs0(void *p) { __asm__ volatile("movl %0, %%fs:0" :: "r"(p)); }

int main(void) {
    /* Two frames in an array so the target (outer, [1]) sits at a HIGHER address than
     * the inner ([0]) — the stack ordering RtlUnwind validates. */
    EReg fr[2];
    EReg *inner = &fr[0], *outer = &fr[1];
    void *prev = get_fs0();
    outer->next = (EReg *)prev;        outer->handler = (void *)h_outer; set_fs0(outer);
    inner->next = (EReg *)get_fs0();   inner->handler = (void *)h_inner; set_fs0(inner);
    g_outer = outer;

    RtlUnwind(outer, NULL, NULL, (PVOID)0);

    g_fs0 = get_fs0();
    set_fs0(prev);                      /* restore the chain */

    printf("inner=%d flags=%#x fs0_target=%d\n", inner_called, inner_flags, g_fs0 == g_outer);
    return 0;
}
