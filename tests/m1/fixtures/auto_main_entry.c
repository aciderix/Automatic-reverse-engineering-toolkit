/* Regression for the auto-`main` entry selection: when the PE entry is a CRT
   bootstrap (`*CRTStartup`) and a distinct `main` symbol exists, the transpiler
   starts at `main` and skips the (unmodelled) native CRT startup.

   The bootstrap here prints a "boot" marker *before* calling main, so the test
   can tell which entry actually ran: starting at the bootstrap prints both
   markers; auto-redirecting to `main` prints only "main". Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O1 -fno-builtin \
       -fno-asynchronous-unwind-tables -fno-stack-protector \
       -e _mainCRTStartup@0 auto_main_entry.c -o auto_main_entry.exe \
       -lmsvcrt -lkernel32
   (the entry is the @0-decorated __stdcall name; `main` keeps the plain cdecl
   symbol, which is what auto-selection looks for.) */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

/* mingw emits a `__main` call (global-ctor runner) at the top of `main`; with
   -nostdlib we provide a no-op so the freestanding binary links. */
void __main(void) {}

int main(void) {
    printf("AUTOENTRY: main\n");
    ExitProcess(0);
    return 0;
}

void __stdcall mainCRTStartup(void) {
    /* A real CRT bootstrap initializes the C runtime then calls main; we model
       only the call (and a marker), since the transpiler should skip straight
       to main and never run this. */
    printf("AUTOENTRY: boot\n");
    main();
    ExitProcess(0);
}
