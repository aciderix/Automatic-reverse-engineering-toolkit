/* Win32 fixture: exercises the native POSIX-backed kernel32 layer (aret_win32.c)
   — heap, atomics, environment, timing, lstr*, paths — proving a real Windows
   program's kernel32 calls run directly on Linux primitives. Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O0 -fno-builtin -e mainCRTStartup \
       hello_win32api.c -o hello_win32api.exe -lmsvcrt -lkernel32  */
#include <windows.h>

__declspec(dllimport) int printf(const char *, ...);

void __stdcall mainCRTStartup(void) {
    /* heap via GetProcessHeap + HeapAlloc(HEAP_ZERO_MEMORY) */
    HANDLE h = GetProcessHeap();
    int *p = (int *)HeapAlloc(h, 0x8, 4 * sizeof(int)); /* zero-init */
    int zero_ok = (p[0] == 0 && p[3] == 0) ? 1 : 0;
    p[0] = 11; p[1] = 22;

    /* interlocked atomics */
    LONG ctr = 0;
    InterlockedIncrement(&ctr);
    InterlockedIncrement(&ctr);
    InterlockedExchangeAdd(&ctr, 40);
    InterlockedDecrement(&ctr);            /* 0+1+1+40-1 = 41 */

    /* environment round-trip */
    SetEnvironmentVariableA("ARET_W32", "ok");
    char ev[16] = {0};
    DWORD evlen = GetEnvironmentVariableA("ARET_W32", ev, sizeof ev);

    /* lstr* helpers */
    char s[32]; lstrcpyA(s, "win"); lstrcatA(s, "32");
    int slen = lstrlenA(s);

    /* timing: monotonic must not go backwards */
    DWORD t0 = GetTickCount();
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    for (volatile int i = 0; i < 100000; i++) {}
    QueryPerformanceCounter(&q1);
    DWORD t1 = GetTickCount();
    int mono_ok = (q1.QuadPart >= q0.QuadPart && t1 >= t0) ? 1 : 0;

    HeapFree(h, 0, p);

    printf("W32: zero=%d ctr=%d ev=%s evlen=%d s=%s slen=%d mono=%d\n",
           zero_ok, (int)ctr, ev, (int)evlen, s, slen, mono_ok);
    ExitProcess(0);
}
