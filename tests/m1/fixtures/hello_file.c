/* Filesystem HLE fixture (UBT Phase 3): write a file through a Windows "C:\"
   path, then read it back and print it. The HLE translates the Windows path to
   a native path under the ARET prefix and maps the stdio calls onto POSIX. */
#include <windows.h>

__declspec(dllimport) int printf(const char *, ...);
__declspec(dllimport) void *fopen(const char *, const char *);
__declspec(dllimport) unsigned fwrite(const void *, unsigned, unsigned, void *);
__declspec(dllimport) unsigned fread(void *, unsigned, unsigned, void *);
__declspec(dllimport) int fclose(void *);
__declspec(dllimport) unsigned strlen(const char *);

void __stdcall mainCRTStartup(void) {
    const char *msg = "FS: round-trip through a C:\\ path\n";
    void *f = fopen("C:\\aret_fs_test.txt", "wb");
    fwrite(msg, 1, strlen(msg), f);
    fclose(f);

    char buf[128];
    void *g = fopen("C:\\aret_fs_test.txt", "rb");
    unsigned n = fread(buf, 1, sizeof(buf) - 1, g);
    buf[n] = 0;
    fclose(g);

    printf("%s", buf);
    ExitProcess(0);
}
