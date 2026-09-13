/* LoadStringW with cchBufferMax==0 — the documented Win32 idiom where lpBuffer is an
 * LPWSTR* that receives a READ-ONLY pointer directly to the (non-NUL-terminated)
 * resource string, and the return value is its length in WCHARs. Real apps use it:
 * notepad fetches its "Ln %d, Col %d" status format this way (LoadStringW(...,0) then
 * wsprintfW). ARET previously returned 0 for cch==0 -> callers formatted an EMPTY
 * string (measured: notepad status bar came out blank). Wine and ARET read the same
 * embedded resource, so length + dereferenced content match bit-for-bit; the pointer
 * VALUE differs (address space) so it is never printed. ASCII content, printed by
 * casting each WCHAR to char, to avoid any %ls locale variance between the engines. */
#include <windows.h>
#include <stdio.h>

static void putws(const WCHAR *p, int n) {   /* print n WCHARs as ASCII (test data is ASCII) */
    for (int i = 0; i < n; i++) putchar((char)(p[i] & 0xFF));
}

int main(void) {
    HMODULE h = GetModuleHandleW(NULL);

    /* Baseline: normal copy path (unchanged behavior). */
    WCHAR buf[64];
    int n = LoadStringW(h, 100, buf, 64);
    printf("copy n=%d [", n); putws(buf, n); printf("]\n");

    /* cch==0 pointer idiom: lpBuffer is an LPWSTR*. */
    const WCHAR *p = NULL;
    int m = LoadStringW(h, 100, (LPWSTR)&p, 0);
    printf("ptr m=%d nonnull=%d [", m, p != NULL);
    if (p) putws(p, m);                         /* not NUL-terminated: exactly m chars */
    printf("]\n");

    /* The notepad idiom: fetch a format string via the pointer, then wsprintfW it. */
    const WCHAR *fmt = NULL;
    int fl = LoadStringW(h, 200, (LPWSTR)&fmt, 0);
    printf("fmt fl=%d\n", fl);
    if (fmt) {
        WCHAR fbuf[64]; int k = fl < 63 ? fl : 63;
        for (int i = 0; i < k; i++) fbuf[i] = fmt[i]; fbuf[k] = 0;
        WCHAR out[128]; wsprintfW(out, fbuf, 1, 1);
        int ol = 0; while (out[ol]) ol++;
        printf("formatted=["); putws(out, ol); printf("]\n");
    }

    /* Missing id -> 0 (pointer left unchanged; we only assert the return). */
    const WCHAR *q = NULL;
    int z = LoadStringW(h, 9999, (LPWSTR)&q, 0);
    printf("missing z=%d\n", z);

    /* ANSI variant: cch==0 canNOT return a pointer to a narrow copy of a Unicode
     * resource, so Wine returns -1 and leaves the buffer untouched. */
    char ab[64]; int an = LoadStringA(h, 100, ab, 64);
    printf("A copy n=%d [%s]\n", an, ab);
    int az = LoadStringA(h, 100, ab, 0);
    printf("A cch0 ret=%d\n", az);

    printf("done\n");
    return 0;
}
