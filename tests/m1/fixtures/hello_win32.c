/* Freestanding Win32 console hello — no CRT. Imports kernel32 only.
   Transpiled by ARET and recompiled natively for Linux in M1. */
#include <windows.h>

void __stdcall mainCRTStartup(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    const char msg[] = "Hello from Windows, running native on Linux\n";
    DWORD written = 0;
    WriteFile(h, msg, sizeof(msg) - 1, &written, 0);
    ExitProcess(0);
}
