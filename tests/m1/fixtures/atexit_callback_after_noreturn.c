/* Minimal reproduction of an address-taken callback (atexit handler) that the
   linear sweep ABSORBED into the preceding function by falling through a
   *noreturn* call, so it was never recovered as its own function -> the atexit
   indirect dispatch aborts on an "unrecovered function". Minimized sqlite3-mingw
   stripped `_sayAbnormalExit` (registered via atexit right after a noreturn
   `_shell_out_of_memory`). The by-value callback position proves it is a
   function, so recovery force-splits the boundary (guarded by
   looks_like_func_start).

   Built stripped so no symbol seeds the callee; the recovery must find it purely
   from the atexit registration + the re-split.

   Build: i686-w64-mingw32-gcc -O2 -s -o atexit_callback_after_noreturn.exe \
          atexit_callback_after_noreturn.c
   Expected (native and ARET): "work 1" then "cleanup 1", clean exit. */
#include <stdio.h>
#include <stdlib.h>

static int g_flag = 1;
/* prologue `mov eax,[g_flag]; test eax,eax` (opcode a1) — the run-once/abnormal
   guard shape; reached only by the atexit indirect dispatch. */
__attribute__((noinline)) static void cleanup(void) { if (g_flag) printf("cleanup %d\n", g_flag); }
__attribute__((noreturn, noinline)) static void die(const char *m) { printf("%s\n", m); exit(0); }
__attribute__((noinline)) void work(int x) { if (x < 0) die("neg"); printf("work %d\n", x); }

int main(int argc, char **argv) {
    (void)argv;
    atexit(cleanup);
    work(argc);
    return 0;
}
