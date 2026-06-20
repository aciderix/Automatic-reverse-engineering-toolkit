/* Win32 fixture (batch 2): system info / paths / sync — the broader kernel32
   surface (aret_win32.c). Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O0 -fno-builtin -e mainCRTStartup \
       hello_win32sys.c -o hello_win32sys.exe -lmsvcrt -lkernel32  */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

void __stdcall mainCRTStartup(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    UINT acp = GetACP();
    char path[260] = {0};
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    char sysdir[260] = {0};
    GetSystemDirectoryA(sysdir, sizeof sysdir);
    int feat = IsProcessorFeaturePresent(23) ? 1 : 0;

    HANDLE m = CreateMutexA(NULL, FALSE, "aret_mtx");
    DWORD w = WaitForSingleObject(m, 0);
    BOOL rel = ReleaseMutex(m);

    printf("W32SYS: page=%lu cpus=%lu acp=%u path=%s sysdir=%s feat=%d mtx=%d wait=%lu rel=%d\n",
           (unsigned long)si.dwPageSize, (unsigned long)si.dwNumberOfProcessors,
           acp, n ? path : "(none)", sysdir, feat, m ? 1 : 0, (unsigned long)w, rel ? 1 : 0);
    ExitProcess(0);
}
