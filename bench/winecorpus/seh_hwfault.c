/* SEH over a HARDWARE fault: a NULL dereference raises an access violation that the
 * installed SEH handler catches. mingw i686 has no __try/__except, so the frame is
 * installed by hand; the handler checks the exception code is STATUS_ACCESS_VIOLATION
 * (0xC0000005) and escapes via longjmp. On Windows/Wine the CPU trap is turned into
 * an SEH dispatch through fs:[0]; ARET must turn the host SIGSEGV into the same
 * dispatch. Expected (Wine and ARET): r=42 code=0xc0000005. */
#include <windows.h>
#include <stdio.h>
#include <setjmp.h>

static jmp_buf g_jb;
static unsigned g_code;
typedef struct EReg { struct EReg *next; void *handler; } EReg;

static EXCEPTION_DISPOSITION __cdecl h(EXCEPTION_RECORD *r, void *fr, CONTEXT *c, void *d) {
    (void)fr; (void)c; (void)d;
    g_code = r->ExceptionCode;
    if (r->ExceptionCode == 0xC0000005u) longjmp(g_jb, 42);   /* access violation: caught */
    return ExceptionContinueSearch;
}
static void *get_fs0(void) { void *p; __asm__ volatile("movl %%fs:0, %0" : "=r"(p)); return p; }
static void  set_fs0(void *p) { __asm__ volatile("movl %0, %%fs:0" :: "r"(p)); }

int main(void) {
    int r = 0;
    EReg reg;
    reg.next = (EReg *)get_fs0(); reg.handler = (void *)h; set_fs0(&reg);
    if (setjmp(g_jb) == 0) {
        volatile int *p = (volatile int *)0;   /* NULL */
        *p = 123;                               /* access violation */
        r = 1;                                  /* not reached */
    } else {
        r = 42;                                 /* caught */
    }
    set_fs0(reg.next);
    printf("r=%d code=%#x\n", r, g_code);
    return 0;
}
