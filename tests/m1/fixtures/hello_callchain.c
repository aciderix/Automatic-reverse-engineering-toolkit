/* M3 fixture: a real internal helper (cdecl, noinline, called twice) that takes
   its arguments on the stack. Exercises the shared machine stack: arguments
   pushed by the caller must reach the callee across the transpiled call. */
#include <windows.h>

static const char LINE1[] = "M3: passed through an internal call (1)\n";
static const char LINE2[] = "M3: passed through an internal call (2)\n";

static void __attribute__((noinline))
emit_line(HANDLE h, const char *s, DWORD n) {
    DWORD written = 0;
    WriteFile(h, s, n, &written, 0);
}

void __stdcall mainCRTStartup(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    emit_line(h, LINE1, sizeof(LINE1) - 1);
    emit_line(h, LINE2, sizeof(LINE2) - 1);
    ExitProcess(0);
}
