/* HLE: msvcrt setlocale("")+CP_ACP (doc 70 §5, P1bis). setlocale(LC_ALL,"") selects
 * the ANSI code page (1252 on the modelled Western install), after which the CRT wide
 * <-> multibyte converters use CP1252 instead of the startup "C" locale. MEASURED
 * bit-for-bit against Wine msvcrt under LC_ALL=C (winediff's env):
 *   - wcstombs is STRICT   : only true CP1252 code points map; U+0100/U+2212 -> EILSEQ;
 *   - wctomb   is BEST-FIT : U+0100->'A', U+2212->'-', but U+4E00 (no best-fit) -> -1.
 * All output is byte-exact hex (no %ls) so ARET and Wine match to the byte. */
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <wchar.h>

static void wst(const wchar_t *w) {
    char b[32]; for (int i = 0; i < 32; i++) b[i] = 0x2a;
    int r = (int)wcstombs(b, w, 32);
    printf("wcstombs r=%d", r);
    if (r >= 0) { printf(" ["); for (int i = 0; i < r; i++) printf("%02x ", (unsigned char)b[i]); printf("]"); }
    printf("\n");
}
static void wct(unsigned wc) {
    char c[8]; for (int i = 0; i < 8; i++) c[i] = 0x2a;
    int r = wctomb(c, (wchar_t)wc);
    printf("wctomb U+%04X r=%d", wc, r);
    if (r > 0) { printf(" b="); for (int i = 0; i < r; i++) printf("%02x", (unsigned char)c[i]); }
    printf("\n");
}

int main(void) {
    printf("start=%s\n", setlocale(LC_ALL, NULL));      /* "C" */
    printf("setl=%s\n", setlocale(LC_ALL, ""));          /* English_United States.1252 */

    wchar_t s1[] = { 0x41, 0x20AC, 0x00E9, 0x0152, 0 };  /* A euro e-acute OE : all map */
    wst(s1);
    wchar_t s2[] = { 0x41, 0x0100, 0x00E9, 0 };          /* strict: writes 'A' then EILSEQ */
    wst(s2);

    wct(0x0041); wct(0x00E9); wct(0x20AC); wct(0x0152); wct(0x2018);
    wct(0x0100); wct(0x2212); wct(0x4E00);               /* best-fit / -1 cases */

    printf("resetC=%s\n", setlocale(LC_CTYPE, "C"));      /* back to "C" */
    wchar_t s3[] = { 0x41, 0x20AC, 0 };                  /* C locale: 'A' then EILSEQ (>0xff) */
    wst(s3);

    printf("done\n");
    return 0;
}
