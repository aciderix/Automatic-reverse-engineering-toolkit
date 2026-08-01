/* shlwapi Str* — wave 2: COPY / CONCATENATE / TRIM / DUPLICATE.
 *
 * Every counted variant takes the size of the WHOLE destination, NUL included —
 * not a count of characters to move. Getting that backwards is a one-character
 * overflow, so the grid sweeps the count instead of sampling it.
 *
 * Buffers are POISONED and dumped raw, because the decisive facts are all about
 * what is NOT written:
 *   - `StrCpyNW(dst, src, 0)` writes nothing at all — not even a NUL. n=1 writes
 *     only the NUL. A test that read the buffer as a string would see neither.
 *   - `StrCatBuff` with a cch that only covers what is already there appends
 *     nothing and leaves the existing string alone.
 *   - `StrCatChainW` writes at ichAt LITERALLY, so a gap before it keeps its old
 *     bytes — the poison in indices 1..4 is the proof, and a string-only check
 *     would report success either way.
 *
 * `StrDup` is checked for CONTENT and for being freeable with LocalFree, which is
 * the pair that has to agree; the pointer itself is never printed.
 *
 * NOT probed: a NULL source to StrNCat/StrCpyN. Wine faults on it (measured), so
 * it is a caller bug, not a contract, and a fixture that crashes proves nothing.
 * `StrCpyNX*` is absent from the mingw import library (undocumented export), so it
 * cannot be bound here without a .def and is left out of this wave.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

static void rawA(const char *what, const char *b, int n, int extra)
{
    printf("%-26s %d [", what, extra);
    for (int i = 0; i < n; i++) printf("%02x", (unsigned char)b[i]);
    printf("]\n");
}
static void rawW(const char *what, const WCHAR *b, int n, int extra)
{
    printf("%-26s %d [", what, extra);
    for (int i = 0; i < n; i++) printf("%04x", b[i]);
    printf("]\n");
}

int main(void)
{
    char a[16];
    WCHAR w[16];
    char nm[48];

    /* StrCatBuff: sweep the total-buffer count across the interesting boundary. */
    for (int cch = 0; cch <= 12; cch++) {
        memset(a, 0xAA, sizeof a); strcpy(a, "ab");
        char *r = StrCatBuffA(a, "CDEFG", cch);
        sprintf(nm, "StrCatBuffA cch=%d", cch);
        rawA(nm, a, 12, r == a);
    }
    memset(w, 0xAA, sizeof w); w[0] = 'a'; w[1] = 'b'; w[2] = 0;
    rawW("StrCatBuffW cch=6", w, 10, StrCatBuffW(w, L"CDEFG", 6) == w);
    memset(a, 0xAA, sizeof a); strcpy(a, "ab");
    rawA("StrCatBuffA empty src", a, 8, StrCatBuffA(a, "", 12) == a);

    /* StrNCat: sweep the append count. */
    for (int n = 0; n <= 8; n++) {
        memset(a, 0xAA, sizeof a); strcpy(a, "ab");
        char *r = StrNCatA(a, "CDEFG", n);
        sprintf(nm, "StrNCatA n=%d", n);
        rawA(nm, a, 12, r == a);
    }
    memset(w, 0xAA, sizeof w); w[0] = 'a'; w[1] = 'b'; w[2] = 0;
    rawW("StrNCatW n=4", w, 10, StrNCatW(w, L"CDEFG", 4) == w);

    /* StrCpyN: sweep the destination size, including 0 and 1. */
    for (int n = 0; n <= 8; n++) {
        memset(w, 0xAA, sizeof w);
        WCHAR *r = StrCpyNW(w, L"hello", n);
        sprintf(nm, "StrCpyNW n=%d", n);
        rawW(nm, w, 8, r == w);
    }
    memset(w, 0xAA, sizeof w);
    rawW("StrCpyW", w, 8, StrCpyW(w, L"hey") == w);
    memset(w, 0xAA, sizeof w); w[0] = 'a'; w[1] = 'b'; w[2] = 0;
    rawW("StrCatW", w, 8, StrCatW(w, L"cd") == w);

    /* StrTrim: both ends, in place, and the BOOL that says whether it moved. */
    { char t[] = "  xxhello worldxx  "; int r = StrTrimA(t, " x");
      printf("StrTrimA          ret=%d \"%s\"\n", r, t); }
    { char t[] = "hello";              int r = StrTrimA(t, " x");
      printf("StrTrimA nochange ret=%d \"%s\"\n", r, t); }
    { char t[] = "xxxx";               int r = StrTrimA(t, "x");
      printf("StrTrimA all      ret=%d len=%d\n", r, (int)strlen(t)); }
    { char t[] = "  ab  ";             int r = StrTrimA(t, "");
      printf("StrTrimA emptyset ret=%d \"%s\"\n", r, t); }
    { char t[] = "  ab  ";             int r = StrTrimA(t, NULL);
      printf("StrTrimA NULLset  ret=%d \"%s\"\n", r, t); }
    printf("StrTrimA NULLstr  ret=%d\n", StrTrimA(NULL, " "));
    { WCHAR t[] = L"  ab  "; int r = StrTrimW(t, L" ");
      printf("StrTrimW          ret=%d \"%ls\"\n", r, t); }

    /* StrDup: content, the empty cases, and LocalFree acceptance. */
    /* Read the content, THEN free — never in the same printf. Argument evaluation
     * order is unspecified, so putting LocalFree beside strcmp is a use-after-free
     * that Wine happens to survive (the bytes are still there) and a different
     * allocator does not. This exact shape produced a false DIFF while writing the
     * fixture; it is the same trap as reading an out-parameter in the call's own
     * printf, and it is worth the three extra lines. */
    { char *p = StrDupA("dup me");
      int eq = p ? strcmp(p, "dup me") == 0 : -1;
      int freed = p ? (LocalFree(p) == NULL) : -1;
      printf("StrDupA null=%d eq=%d freed=%d\n", p == NULL, eq, freed); }
    { char *p = StrDupA(NULL);
      printf("StrDupA(NULL) null=%d len=%d\n", p == NULL, p ? (int)strlen(p) : -1);
      if (p) LocalFree(p); }
    { char *p = StrDupA("");
      printf("StrDupA(\"\") null=%d len=%d\n", p == NULL, p ? (int)strlen(p) : -1);
      if (p) LocalFree(p); }
    { WCHAR *p = StrDupW(L"wide");
      printf("StrDupW null=%d eq=%d\n", p == NULL, p ? wcscmp(p, L"wide") == 0 : -1);
      if (p) LocalFree(p); }
    { WCHAR *p = StrDupW(NULL);
      printf("StrDupW(NULL) null=%d len=%d\n", p == NULL, p ? (int)wcslen(p) : -1);
      if (p) LocalFree(p); }

    /* StrCatChainW: the returned index, a literal ichAt, overflow, and cch 0. */
    { WCHAR c[16]; memset(c, 0xAA, sizeof c); c[0] = 0;
      DWORD o = StrCatChainW(c, 8, 0, L"abc");        rawW("chain at=0", c, 10, (int)o);
      o = StrCatChainW(c, 8, o, L"defghij");          rawW("chain at=end", c, 10, (int)o);
      o = StrCatChainW(c, 8, (DWORD)-1, L"Z");        rawW("chain at=-1 full", c, 10, (int)o); }
    { WCHAR c[16]; memset(c, 0xAA, sizeof c); c[0] = 0;
      DWORD o = StrCatChainW(c, 8, 5, L"XY");         rawW("chain at=5 gap", c, 10, (int)o); }
    { WCHAR c[16]; memset(c, 0xAA, sizeof c); c[0] = 0;
      DWORD o = StrCatChainW(c, 8, 0, L"abcdefghij"); rawW("chain overflow", c, 10, (int)o); }
    { WCHAR c[16]; memset(c, 0xAA, sizeof c); c[0] = 0;
      DWORD o = StrCatChainW(c, 0, 0, L"abc");
      printf("chain cch=0 o=%lu u0=%04x\n", (unsigned long)o, c[0]); }
    return 0;
}
