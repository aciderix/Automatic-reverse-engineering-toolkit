/* Regression for Bug #2: a loop whose exit edge carries a *value* (the result is
   computed on the block reached by leaving the loop), with a SECOND exit edge to
   a different block. The structurer must not collapse the value-carrying exit
   into a bare `break` that diverts to the other exit's follow block — doing so
   drops the loop-carried result and the mismatch path returns the wrong value.

   `mystrcmp` is the canonical case: the "mismatch" exit computes (ca<cb)?-1:1 via
   the sbb/or idiom on the way out; the "match" exit returns 0. Both leave the
   `for(;;)` to different result blocks. Expected: eq=0 lt<0 gt>0. Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O1 -fno-builtin \
       -fno-asynchronous-unwind-tables -fno-stack-protector \
       -e _mainCRTStartup@0 loop_exit_value.c -o loop_exit_value.exe \
       -lmsvcrt -lkernel32
   (the entry is the @0-decorated __stdcall name, otherwise ld defaults the PE
   entry to the first function, mystrcmp, and the program runs the wrong code.) */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

static int mystrcmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca != cb) return (ca < cb) ? -1 : 1;  /* mismatch: result on exit edge */
        if (ca == 0) return 0;                     /* match */
    }
}

void __stdcall mainCRTStartup(void) {
    int eq = mystrcmp("hello", "hello");
    int lt = mystrcmp("abc", "abd");
    int gt = mystrcmp("abd", "abc");
    /* Normalize the sign so the expected string is stable across the sbb/or
       idiom's exact magnitude. */
    printf("LOOPEXIT: eq=%d lt=%d gt=%d\n",
           eq == 0 ? 0 : 9, lt < 0 ? -1 : 9, gt > 0 ? 1 : 9);
    ExitProcess(0);
}
