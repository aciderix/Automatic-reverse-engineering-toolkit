/* CRT fixture: exercises the libc-backed CRT forwarding layer (aret_crt.c) —
   string building, search, case-insensitive compare, ctype, integer conversion,
   and sprintf — to prove the genuine host C runtime is reached ABI-accurately
   through the shared machine stack. Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -Os -e mainCRTStartup \
       hello_crt.c -o hello_crt.exe -lmsvcrt -lkernel32  */
#include <windows.h>

__declspec(dllimport) int printf(const char *, ...);
__declspec(dllimport) int sprintf(char *, const char *, ...);
__declspec(dllimport) char *strncpy(char *, const char *, unsigned);
__declspec(dllimport) char *strcat(char *, const char *);
__declspec(dllimport) char *strrchr(const char *, int);
__declspec(dllimport) char *strstr(const char *, const char *);
__declspec(dllimport) int memcmp(const void *, const void *, unsigned);
__declspec(dllimport) int _stricmp(const char *, const char *);
__declspec(dllimport) int toupper(int);
__declspec(dllimport) int isdigit(int);
__declspec(dllimport) long atol(const char *);
__declspec(dllimport) long strtol(const char *, char **, int);
__declspec(dllimport) int abs(int);

void __stdcall mainCRTStartup(void) {
    char buf[64];
    /* string building */
    strncpy(buf, "rev", 4);   /* "rev" + NUL */
    strcat(buf, "erse");      /* "reverse" */

    /* search */
    const char *dot = strrchr("a.b.c", '.');   /* -> ".c" */
    const char *sub = strstr("transpiler", "pile"); /* -> "piler" */

    /* case-insensitive compare + ctype + conversions */
    int ci = _stricmp("ARET", "aret");          /* 0 */
    int up = toupper('z');                       /* 'Z' */
    int dig = isdigit('7') ? 1 : 0;              /* 1 */
    long n = atol("  -123");                     /* -123 */
    long hexv = strtol("ff", 0, 16);             /* 255 */
    int av = abs(-42);                           /* 42 */
    int mc = memcmp("abc", "abd", 3);            /* < 0 */

    char line[128];
    sprintf(line, "CRT: s=%s dot=%s sub=%s ci=%d up=%c dig=%d n=%ld hex=%ld abs=%d mc=%d",
            buf, dot, sub, ci, up, dig, n, hexv, av, mc < 0 ? 1 : 0);
    printf("%s\n", line);
    ExitProcess(0);
}
