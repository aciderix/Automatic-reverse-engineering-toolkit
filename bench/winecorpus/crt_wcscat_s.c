/* wcscat_s — the MSVC/Annex K secure wide append (reached by real MFC start-up code).
 * Its error behaviour is observable in the DESTINATION, not just the return code, so
 * every case dumps the raw 16-bit buffer after a poison fill:
 *   - destsz == 0            -> EINVAL(22), destination left UNTOUCHED (not emptied);
 *   - src == NULL            -> EINVAL(22), dest[0] = 0;
 *   - dest unterminated      -> ERANGE(34), dest[0] = 0;
 *   - result does not fit    -> ERANGE(34), dest[0] = 0, but the elements the copy had
 *                               already written stay CLOBBERED (a real, visible effect
 *                               that an idealised implementation would hide);
 *   - exact fit (NUL on the last element) succeeds.
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static void show(const char *tag, int r, const wchar_t *b, int n) {
    printf("%-8s r=%d raw=", tag, r);
    for (int i = 0; i < n; i++) printf("%04x ", (unsigned)(unsigned short)b[i]);
    printf("\n");
}

int main(void) {
    wchar_t b[8];

    wmemset(b, 0x2020, 8); wcscpy(b, L"ab");
    show("fit", wcscat_s(b, 8, L"cd"), b, 8);

    wmemset(b, 0x2020, 8); wcscpy(b, L"ab");
    show("exact", wcscat_s(b, 6, L"cde"), b, 8);   /* 5 chars + NUL == 6 */

    wmemset(b, 0x2020, 8); wcscpy(b, L"ab");
    show("range", wcscat_s(b, 5, L"cde"), b, 8);   /* one element too few */

    wmemset(b, 0x2020, 8); wcscpy(b, L"ab");
    show("size0", wcscat_s(b, 0, L"x"), b, 8);

    wmemset(b, 0x2020, 8); wcscpy(b, L"ab");
    show("nullsrc", wcscat_s(b, 8, NULL), b, 8);

    printf("nulldst  r=%d\n", wcscat_s(NULL, 8, L"x"));

    wmemset(b, 0x2020, 8);                          /* no terminator within destsz */
    show("noterm", wcscat_s(b, 4, L"z"), b, 8);

    return 0;
}
