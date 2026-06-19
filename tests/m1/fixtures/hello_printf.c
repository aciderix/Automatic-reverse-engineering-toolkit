/* M4 fixture: a console program using the C runtime (msvcrt) — printf with
   several conversions, plus malloc/free. The imports are intercepted into native
   HLE shims; printf is implemented by reading its variadic arguments from the
   shared machine stack. */
#include <windows.h>

__declspec(dllimport) int printf(const char *, ...);
__declspec(dllimport) void *malloc(unsigned int);
__declspec(dllimport) void free(void *);

void __stdcall mainCRTStartup(void) {
    printf("M4: int=%d hex=0x%x str=%s char=%c pct=%%\n", 42, 255, "hello", 'Z');
    int *p = (int *)malloc(4 * sizeof(int));
    p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 40;
    printf("M4: malloc sum=%d\n", p[0] + p[1] + p[2] + p[3]);
    free(p);
    ExitProcess(0);
}
