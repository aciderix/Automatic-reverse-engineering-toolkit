/* sprintf_s / vsprintf_s — a narrow twin that does not behave like its wide one.
 *
 * ARET implements `swprintf_s`/`vswprintf_s` (winecorpus/crt_swprintf_s.c) whose
 * Wine behaviour is clean and self-consistent: capacity 0 leaves the buffer alone;
 * a capacity too small zero-fills exactly `capacity` units and returns -1; a fit
 * writes the text plus its NUL and returns the length.
 *
 * The ANSI twins, forced to msvcrt through a .def and measured under Wine, do
 * something else on BOTH sides of that:
 *
 *   1. On failure they leave PARTIAL OUTPUT in the buffer ("4", "42", "42-", …)
 *      instead of zero-filling it. A caller that treats -1 as "buffer untouched"
 *      then reads an unterminated fragment.
 *   2. At an EXACT fit (capacity 5 for a 5-character result) they return 5 — a
 *      success — while writing five characters and NO terminator. No caller can use
 *      that safely, and it contradicts the wide version, which needs capacity 6 for
 *      the same string.
 *
 * Two functions of one family disagreeing about where the NUL goes is the signature
 * of an implementation slip, not of a contract — the same shape as PathIsUNCServerA
 * and as StrChrW on the terminator. So ARET does NOT implement them: they stay a
 * loud abort until this runner says what Windows actually does. If Windows matches
 * the wide behaviour, the wide code serves both; if Windows really does return a
 * length without terminating, that is worth knowing before any program depends on it.
 *
 * The .def is what makes this measurement mean anything: mingw supplies its own
 * bodies for several `*_s` functions, and without forcing the import both sides
 * would be measuring mingw (doc 70 section 7). The import table is printed first so
 * the log itself shows the calls really went to the CRT. */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

int __cdecl sprintf_s(char *, size_t, const char *, ...);
int __cdecl vsprintf_s(char *, size_t, const char *, va_list);

static int callv(char *b, size_t n, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int r = vsprintf_s(b, n, f, ap);
    va_end(ap);
    return r;
}

static void dump(const char *what, int cap, int r, const char *b, int n)
{
    printf("%-12s cap=%-2d r=%-3d [", what, cap, r);
    for (int i = 0; i < n; i++) printf("%02x", (unsigned char)b[i]);
    printf("]\n");
}

int main(void)
{
    char a[16];
    for (int cap = 0; cap <= 8; cap++) {
        memset(a, 0xAA, sizeof a);
        int r = sprintf_s(a, cap, "%d-%s", 42, "xy");   /* "42-xy", 5 chars */
        dump("sprintf_s", cap, r, a, 8);
    }
    for (int cap = 0; cap <= 8; cap++) {
        memset(a, 0xAA, sizeof a);
        int r = callv(a, cap, "%d-%s", 42, "xy");
        dump("vsprintf_s", cap, r, a, 8);
    }
    memset(a, 0xAA, sizeof a);
    { int r = sprintf_s(a, 4, "");
      printf("empty r=%d b0=%02x b1=%02x\n", r, (unsigned char)a[0], (unsigned char)a[1]); }
    return 0;
}
