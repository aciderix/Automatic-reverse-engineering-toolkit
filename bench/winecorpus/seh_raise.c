/* Structured Exception Handling — software-raised dispatch.
 *
 * mingw i686 has no `__try/__except`, so the SEH frame is installed by hand (the
 * exact prologue MSVC emits): an EXCEPTION_REGISTRATION { next; handler } on the
 * stack, linked into `fs:[0]`. `RaiseException` must walk that chain and call each
 * handler cdecl; a handler that declines returns ExceptionContinueSearch and the
 * next frame is tried; one that catches transfers control non-locally (here a
 * `longjmp`). Two nested frames exercise the chain walk: the inner handler
 * declines, the outer catches. Expected (Wine and ARET): r=42.
 */
#include <windows.h>
#include <stdio.h>
#include <setjmp.h>

static jmp_buf g_jb;
typedef struct EReg { struct EReg *next; void *handler; } EReg;

static EXCEPTION_DISPOSITION h_inner(EXCEPTION_RECORD *r, void *fr, CONTEXT *c, void *d) {
    (void)fr; (void)c; (void)d;
    return ExceptionContinueSearch; /* decline: dispatcher tries the outer frame */
}
static EXCEPTION_DISPOSITION h_outer(EXCEPTION_RECORD *r, void *fr, CONTEXT *c, void *d) {
    (void)fr; (void)c; (void)d;
    if (r->ExceptionCode == 0xE0001234u) longjmp(g_jb, 42);
    return ExceptionContinueSearch;
}
static void push_seh(EReg *reg, void *h) {
    void *prev;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(prev));
    reg->next = (EReg *)prev;
    reg->handler = h;
    __asm__ volatile("movl %0, %%fs:0" :: "r"(reg));
}
static void pop_seh(EReg *reg) {
    __asm__ volatile("movl %0, %%fs:0" :: "r"(reg->next));
}

int main(void) {
    int r = 0;
    EReg outer, inner;
    push_seh(&outer, (void *)h_outer);
    push_seh(&inner, (void *)h_inner);
    if (setjmp(g_jb) == 0) {
        RaiseException(0xE0001234u, 0, 0, NULL);
        r = 1; /* not reached: the exception unwinds to the outer handler */
    } else {
        r = 42; /* caught */
    }
    pop_seh(&inner);
    pop_seh(&outer);
    printf("r=%d\n", r);
    return 0;
}
