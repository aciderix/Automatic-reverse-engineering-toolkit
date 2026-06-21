#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);
/* Countdown loop terminating on `i != -1` (cmp r32, -1; jne). The immediate -1
   is sign-extended to 64-bit; the 32-bit counter reaches 0xffffffff and must
   compare equal to it. If the immediate isn't truncated to the counter width the
   loop never terminates (the bug behind lua_pushcclosure's upvalue loop). */
static int __attribute__((noinline)) sumdown(int n) {
    int s = 0;
    for (int i = n - 1; i != -1; i--) s += i;
    return s;
}
void __stdcall mainCRTStartup(void) {
    printf("EQIMM: %d %d\n", sumdown(5), sumdown(1));  /* 10 0 */
    ExitProcess(0);
}
