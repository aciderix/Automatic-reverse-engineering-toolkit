/* M4 fixture: heap allocation + string runtime. malloc/free/strlen are CRT
   imports shimmed by the HLE; an internal noinline helper (shared machine stack)
   fills the buffer so the compiler cannot fold it away. */
#include <windows.h>

__declspec(dllimport) int printf(const char *, ...);
__declspec(dllimport) void *malloc(unsigned int);
__declspec(dllimport) void free(void *);
__declspec(dllimport) unsigned int strlen(const char *);

static void __attribute__((noinline)) fill(char *p, int n) {
    int i;
    for (i = 0; i < n; i++) p[i] = (char)('A' + i);
    p[n] = 0;
}

void __stdcall mainCRTStartup(void) {
    char *p = (char *)malloc(16);
    fill(p, 5);
    printf("M4: heap=%s len=%u\n", p, strlen(p));
    free(p);
    ExitProcess(0);
}
