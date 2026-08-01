/* shlwapi Str* — wave 1: the SEARCH/SCAN group.
 *
 * These look like <string.h> and are NOT <string.h>. Every group below is a pair of
 * rows chosen so the shlwapi answer and the obvious C answer DIFFER — a fixture that
 * only checked "finds the substring" would pass against a wrong implementation.
 *
 *   - An EMPTY needle returns NULL here; `strstr` returns the haystack.
 *   - `StrCSpn(s,"")` is the FULL length while `StrSpn(s,"")` is 0.
 *   - The `N` variants bound where a match may START, not where it may end, so the
 *     n sweep below (0..22 in the derivation) is what pins the rule down.
 *   - Every function survives NULL arguments.
 *   - ⚠️ `StrChrW(s,0)` returns the TERMINATOR while `StrChrA(s,'\0')`, `StrChrIW`
 *     and `StrRChrW` all return NULL. One of four disagrees with its siblings. That
 *     is reproduced verbatim because Wine is the gate, and it is QUEUED FOR THE
 *     WINDOWS ORACLE (bench/winoracle/win32_strdisputed.c) because it looks like a
 *     Wine slip. Do not "fix" it toward consistency without a Windows measurement.
 *
 * Results are pointers INTO the input, so OFFSETS are printed, never pointers: an
 * address differs between two honest runs and would make this fixture flap.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>

static const char *A = "Hello, World! Hello.";
static const WCHAR *W = L"Hello, World! Hello.";

static void offA(const char *what, const char *base, const char *r)
{ printf("%-28s %d\n", what, r ? (int)(r - base) : -1); }
static void offW(const char *what, const WCHAR *base, const WCHAR *r)
{ printf("%-28s %d\n", what, r ? (int)(r - base) : -1); }
static void num(const char *what, int v) { printf("%-28s %d\n", what, v); }

int main(void)
{
    /* The n sweep: the only thing that distinguishes "start bounded by n" from
     * "whole match bounded by n" (a 5-char needle at index 7 is found at n=8). */
    for (int n = 0; n <= 22; n++) {
        char nm[48];
        LPWSTR r = StrStrNW(W, L"World", n);
        sprintf(nm, "StrStrNW n=%d", n);
        offW(nm, W, r);
        r = StrChrNW(W, 'W', n);
        sprintf(nm, "StrChrNW n=%d", n);
        offW(nm, W, r);
    }
    /* --- StrChr / StrChrI / StrRChr / StrRChrI --------------------------- */
    offA("StrChrA 'o'",        A, StrChrA(A, 'o'));
    offA("StrChrA 'z'",        A, StrChrA(A, 'z'));
    offA("StrChrA NUL",        A, StrChrA(A, '\0'));
    offA("StrChrA 'H'",        A, StrChrA(A, 'H'));
    offA("StrChrIA 'h'",       A, StrChrIA(A, 'h'));
    offA("StrChrIA 'W'",       A, StrChrIA(A, 'w'));
    offA("StrRChrA 'o'",       A, StrRChrA(A, NULL, 'o'));
    offA("StrRChrA 'H' end@5", A, StrRChrA(A, A + 5, 'H'));
    offA("StrRChrA 'z'",       A, StrRChrA(A, NULL, 'z'));
    offA("StrRChrIA 'h'",      A, StrRChrIA(A, NULL, 'h'));
    offW("StrChrW 'o'",        W, StrChrW(W, 'o'));
    offW("StrChrW 'z'",        W, StrChrW(W, 'z'));
    offW("StrChrW NUL",        W, StrChrW(W, 0));
    offW("StrChrIW 'h'",       W, StrChrIW(W, 'h'));
    offW("StrChrNW 'o' n=4",   W, StrChrNW(W, 'o', 4));
    offW("StrChrNW 'o' n=6",   W, StrChrNW(W, 'o', 6));
    offW("StrRChrW 'o'",       W, StrRChrW(W, NULL, 'o'));
    offW("StrRChrIW 'h'",      W, StrRChrIW(W, NULL, 'h'));

    /* --- StrStr / StrStrI / StrStrN / StrRStrI --------------------------- */
    offA("StrStrA 'World'",    A, StrStrA(A, "World"));
    offA("StrStrA 'world'",    A, StrStrA(A, "world"));
    offA("StrStrA empty",      A, StrStrA(A, ""));
    offA("StrStrA 'Hello'",    A, StrStrA(A, "Hello"));
    offA("StrStrIA 'world'",   A, StrStrIA(A, "world"));
    offA("StrStrIA empty",     A, StrStrIA(A, ""));
    offA("StrRStrIA 'hello'",  A, StrRStrIA(A, NULL, "hello"));
    offA("StrRStrIA end@10",   A, StrRStrIA(A, A + 10, "hello"));
    offA("StrRStrIA empty",    A, StrRStrIA(A, NULL, ""));
    offW("StrStrW 'World'",    W, StrStrW(W, L"World"));
    offW("StrStrW empty",      W, StrStrW(W, L""));
    offW("StrStrIW 'world'",   W, StrStrIW(W, L"world"));
    offW("StrStrNW n=8",       W, StrStrNW(W, L"World", 8));
    offW("StrStrNW n=13",      W, StrStrNW(W, L"World", 13));
    offW("StrStrNIW n=13",     W, StrStrNIW(W, L"world", 13));
    offW("StrRStrIW 'hello'",  W, StrRStrIW(W, NULL, L"hello"));

    /* --- StrSpn / StrCSpn / StrCSpnI / StrPBrk --------------------------- */
    num("StrSpnA 'Hel'",       StrSpnA(A, "Hel"));
    num("StrSpnA 'xyz'",       StrSpnA(A, "xyz"));
    num("StrSpnA empty",       StrSpnA(A, ""));
    num("StrSpnA all",         StrSpnA("aaa", "a"));
    num("StrCSpnA 'W'",        StrCSpnA(A, "W"));
    num("StrCSpnA 'xyz'",      StrCSpnA(A, "xyz"));
    num("StrCSpnA empty",      StrCSpnA(A, ""));
    num("StrCSpnIA 'w'",       StrCSpnIA(A, "w"));
    num("StrSpnW 'Hel'",       StrSpnW(W, L"Hel"));
    num("StrSpnW 'xyz'",       StrSpnW(W, L"xyz"));
    num("StrSpnW empty",       StrSpnW(W, L""));
    num("StrCSpnW 'W'",        StrCSpnW(W, L"W"));
    num("StrCSpnIW 'w'",       StrCSpnIW(W, L"w"));
    offA("StrPBrkA 'zW'",      A, StrPBrkA(A, "zW"));
    offA("StrPBrkA 'xyz'",     A, StrPBrkA(A, "xyz"));
    offA("StrPBrkA empty",     A, StrPBrkA(A, ""));
    offW("StrPBrkW 'zW'",      W, StrPBrkW(W, L"zW"));
    offW("StrPBrkW empty",     W, StrPBrkW(W, L""));

    /* --- NULL arguments: does it survive, and with what? ----------------- */
    offA("StrChrA(NULL)",      A, StrChrA(NULL, 'o'));
    offA("StrStrA(NULL,x)",    A, StrStrA(NULL, "x"));
    offA("StrStrA(x,NULL)",    A, StrStrA(A, NULL));
    offW("StrStrW(NULL,x)",    W, StrStrW(NULL, L"x"));
    offW("StrStrW(x,NULL)",    W, StrStrW(W, NULL));
    num("StrSpnA(NULL,x)",     StrSpnA(NULL, "x"));
    num("StrSpnA(x,NULL)",     StrSpnA(A, NULL));
    num("StrSpnW(NULL,x)",     StrSpnW(NULL, L"x"));
    num("StrSpnW(x,NULL)",     StrSpnW(W, NULL));
    offA("StrPBrkA(NULL,x)",   A, StrPBrkA(NULL, "x"));
    offA("StrPBrkA(x,NULL)",   A, StrPBrkA(A, NULL));
    offA("StrRChrA(NULL)",     A, StrRChrA(NULL, NULL, 'o'));
    offW("StrRStrIW(NULL,x)",  W, StrRStrIW(NULL, NULL, L"x"));
    offW("StrRStrIW(x,NULL)",  W, StrRStrIW(W, NULL, NULL));
    return 0;
}
