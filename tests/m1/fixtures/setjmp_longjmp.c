/* setjmp/longjmp through the lifter. msvcrt's setjmp arrives as `_setjmp3`
   reached via an *import thunk* (`jmp *[IAT]`), so this exercises two things:
     1. import-thunk resolution — `call <thunk>` must bind to the import shim at
        the real call site, not vanish into the throwaway thunk;
     2. the setjmp/longjmp intrinsics — expanded as macros at the lifted call
        site so the host setjmp/longjmp run in the lifted function's own native
        frame. longjmp here unwinds five deep() frames back to the setjmp.
   Expected: caught=42 (the value passed to longjmp). Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O1 -fno-builtin \
       -fno-asynchronous-unwind-tables -fno-stack-protector \
       -e _mainCRTStartup@0 setjmp_longjmp.c -o setjmp_longjmp.exe \
       -lmsvcrt -lkernel32  */
#include <windows.h>
#include <setjmp.h>
__declspec(dllimport) int printf(const char *, ...);

static jmp_buf jb;

static int __attribute__((noinline)) deep(int n) {
    if (n == 0) longjmp(jb, 42);   /* non-local exit, carrying a value */
    return deep(n - 1) + 1;        /* never returns normally */
}

void __stdcall mainCRTStartup(void) {
    int r = setjmp(jb);
    if (r == 0) { deep(5); printf("SETJMP: unreachable\n"); }
    else        { printf("SETJMP: caught=%d\n", r); }   /* expect 42 */
    ExitProcess(0);
}
