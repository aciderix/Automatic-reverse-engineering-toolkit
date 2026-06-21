/* Regression for `find_main` on a *stripped* CRT binary: the startup makes a
   3-stack-arg call to a CRT-style helper BEFORE `call main`. The naive finder
   returns the first such call (the decoy); the correct one is distinguished by
   saving its result as the process exit code. With no symbols, discovery must
   still land on `main`, not the decoy. Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O1 -fno-builtin \
       -fno-asynchronous-unwind-tables -fno-stack-protector \
       -e _mainCRTStartup@0 crt_main_discovery.c -o crt_main_discovery.exe \
       -lmsvcrt -lkernel32
     i686-w64-mingw32-strip crt_main_discovery.exe
   (entry = the @0-decorated __stdcall name; stripped so discovery runs the
   call-pattern heuristic, not the symbol table. `noinline` + `volatile` keep the
   calls from being inlined/const-folded away by -O1.) */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

void __main(void) {}

static volatile int g_argc = 1;
static char *g_argv[2];
static volatile int g_ret;

/* A CRT-helper-shaped decoy: 3 stack args, result consumed by a branch. */
__attribute__((noinline)) int decoy(int a, int b, int c) {
    return a + b + c;
}

__attribute__((noinline)) int main(int argc, char **argv, char **envp) {
    (void)argv;
    (void)envp;
    printf("MAIN argc=%d\n", argc);
    return argc + 6;
}

void __stdcall mainCRTStartup(void) {
    /* Decoy first — the naive finder would stop here. Its result feeds a branch,
       it is never saved, so the heuristic rejects it. */
    int d = decoy(g_argc, 2, 3);
    if (d == 999) printf("x");
    /* Real main: args on the stack, result saved as the exit code. */
    g_ret = main(g_argc, g_argv, 0);
    ExitProcess(g_ret);
}
