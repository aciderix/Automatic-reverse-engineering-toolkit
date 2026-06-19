/* M3 fixture (shared machine stack): an internal helper with 6 arguments. With
   gcc's -O1 regparm(3) the first three arrive in registers (eax/edx/ecx) and
   arguments 4-6 are passed on the stack. Reading the stack arguments forces a
   real cross-function stack transfer — the heart of the shared machine stack. */
#include <windows.h>

static const char GREET[] = "M3: argument arrived via the shared STACK\n";

static void __attribute__((noinline))
emit6(HANDLE h, const char *s, int a4, int a5, int a6, DWORD n) {
    DWORD written = 0;
    if (a4 + a5 + a6 == 3) /* uses stack arguments 4, 5, 6 */
        WriteFile(h, s, n, &written, 0);
}

void __stdcall mainCRTStartup(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    emit6(h, GREET, 1, 1, 1, sizeof(GREET) - 1);
    ExitProcess(0);
}
