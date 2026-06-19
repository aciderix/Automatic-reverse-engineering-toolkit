/* Floating-point fixture: SSE double arithmetic at runtime (volatile defeats
   constant folding) + conversion to int. Exercises the __fp_* runtime helpers. */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

void __stdcall mainCRTStartup(void) {
    volatile double a = 3.5, b = 2.0;
    double c = a * b + 1.5;            /* 8.5 */
    printf("FLOAT: c=%d c10=%d\n", (int)c, (int)(c * 10.0));  /* 8 85 */
    ExitProcess(0);
}
