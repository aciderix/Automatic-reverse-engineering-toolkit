/* HLE family: msvcrt wide locale/stdio (measured OS wall, doc 82) -- wcsxfrm/strxfrm
 * (C-locale collation = identity), _wcsftime (wide strftime), and getwc/putwc/ungetwc
 * (wide stdio, one byte per wchar in the default C locale). Prints wide results
 * char-by-char (avoids %ls in the HLE printf) so ARET and Wine (same msvcrt) match
 * bit-for-bit. */
#include <stdio.h>
#include <wchar.h>
#include <time.h>
#include <string.h>

static void pw(const wchar_t *w) { for (; *w; w++) putchar((int)*w); }

int main(void) {
    /* putwc / getwc round-trip through a temp file */
    FILE *f = fopen("aretwc.txt", "wb");
    for (wchar_t c = L'A'; c <= L'E'; c++) putwc(c, f);
    fclose(f);
    f = fopen("aretwc.txt", "rb");
    wint_t c; int n = 0;
    while ((c = getwc(f)) != WEOF) { putchar((int)c); n++; }
    printf("|n=%d\n", n);
    fclose(f);

    /* ungetwc: read one, push it back, read again */
    f = fopen("aretwc.txt", "rb");
    wint_t a = getwc(f);
    ungetwc(a, f);
    wint_t b = getwc(f);
    printf("unget=%c%c\n", (int)a, (int)b);
    fclose(f);
    remove("aretwc.txt");

    /* wcsxfrm / strxfrm: C-locale identity transform */
    wchar_t wb[16]; size_t wr = wcsxfrm(wb, L"hello", 16);
    printf("wcsxfrm="); pw(wb); printf(" len=%d\n", (int)wr);
    char sb[16]; size_t sr = strxfrm(sb, "hello", 16);
    printf("strxfrm=%s len=%d\n", sb, (int)sr);

    /* wcsftime: a fixed struct tm -> deterministic formatted string */
    struct tm t; memset(&t, 0, sizeof t);
    t.tm_year = 124; t.tm_mon = 0; t.tm_mday = 15;
    t.tm_hour = 13; t.tm_min = 45; t.tm_sec = 30; t.tm_wday = 1; t.tm_yday = 14;
    wchar_t tb[64]; size_t tr = wcsftime(tb, 64, L"%Y-%m-%d %H:%M:%S", &t);
    printf("wcsftime="); pw(tb); printf(" len=%d\n", (int)tr);

    printf("done\n");
    return 0;
}
