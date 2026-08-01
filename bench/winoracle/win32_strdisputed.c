/* shlwapi Str* — the cells where Wine may not be the right answer.
 *
 * The search/scan family was measured exhaustively against Wine and shipped
 * (winecorpus/str_search.c). Three results in that measurement look like Wine
 * rather than like Win32, and Wine cannot settle them because Wine is the suspect:
 *
 *   1. `StrChrW(s, 0)` returns the TERMINATOR while `StrChrA(s,'\0')`,
 *      `StrChrIW(s,0)` and `StrRChrW(s,NULL,0)` all return NULL. One function out of
 *      four disagreeing with its own siblings is the signature of a slip, not a
 *      contract — exactly how PathIsUNCServerA looked before the runner settled it.
 *   2. An EMPTY needle returns NULL from StrStr/StrStrI/StrRStrI, where the C
 *      library's strstr returns the haystack. If Windows follows the C convention,
 *      every caller that passes a computed (possibly empty) needle takes a different
 *      branch under us.
 *   3. The `N` variants bound where a match may START, not where it may END (a
 *      5-character needle at index 7 is found with n=8). The sweep below is the
 *      whole discriminating range, so the runner's answer is unambiguous.
 *
 * Offsets are printed, never pointers: an address differs between honest runs. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>

static const char  *A = "Hello, World! Hello.";     /* "World" at 7..11 */
static const WCHAR *W = L"Hello, World! Hello.";

static void offA(const char *what, const char *r)
{ printf("%-26s %d\n", what, r ? (int)(r - A) : -1); }
static void offW(const char *what, const WCHAR *r)
{ printf("%-26s %d\n", what, r ? (int)(r - W) : -1); }

int main(void)
{
    /* 1. The terminator, all four ways. */
    offA("StrChrA NUL",   StrChrA(A, '\0'));
    offW("StrChrW NUL",   StrChrW(W, 0));
    offA("StrChrIA NUL",  StrChrIA(A, '\0'));
    offW("StrChrIW NUL",  StrChrIW(W, 0));
    offA("StrRChrA NUL",  StrRChrA(A, NULL, '\0'));
    offW("StrRChrW NUL",  StrRChrW(W, NULL, 0));

    /* 2. The empty needle, every search function, both widths. */
    offA("StrStrA empty",     StrStrA(A, ""));
    offW("StrStrW empty",     StrStrW(W, L""));
    offA("StrStrIA empty",    StrStrIA(A, ""));
    offW("StrStrIW empty",    StrStrIW(W, L""));
    offA("StrRStrIA empty",   StrRStrIA(A, NULL, ""));
    offW("StrRStrIW empty",   StrRStrIW(W, NULL, L""));
    offA("StrPBrkA empty",    StrPBrkA(A, ""));
    offW("StrPBrkW empty",    StrPBrkW(W, L""));
    printf("%-26s %d\n", "StrSpnA empty",  StrSpnA(A, ""));
    printf("%-26s %d\n", "StrCSpnA empty", StrCSpnA(A, ""));

    /* 3. The n rule, across the whole discriminating range. */
    for (int n = 5; n <= 13; n++) {
        char nm[48];
        sprintf(nm, "StrStrNW n=%d", n);
        offW(nm, StrStrNW(W, L"World", n));
        sprintf(nm, "StrChrNW n=%d", n);
        offW(nm, StrChrNW(W, 'W', n));
    }

    /* NULL tolerance: a fault on the runner would itself be the finding. */
    offA("StrStrA(NULL,x)",   StrStrA(NULL, "x"));
    offA("StrStrA(x,NULL)",   StrStrA(A, NULL));
    printf("%-26s %d\n", "StrSpnA(x,NULL)", StrSpnA(A, NULL));
    return 0;
}
