/* _wcsicoll / wcscoll — the locale-collating wide compares (reached by real MFC
 * start-up code). In the default "C" locale — the one a program is in before it calls
 * setlocale — msvcrt collates ORDINALLY: _wcsicoll is exactly _wcsicmp and wcscoll is
 * exactly wcscmp.
 *
 * That is worth pinning down with a test, because the intuitive wiring is wrong: the
 * name says "collate", which suggests the linguistic sort-key path behind lstrcmpiW /
 * CompareStringW. The pairs below are chosen to DISCRIMINATE the two: for "readme" vs
 * "read-me", "~" vs "a" and "O'Brien" vs "OBrien", the linguistic order is the
 * opposite of the ordinal one, so an implementation that routed _wcsicoll to the
 * linguistic machinery would show up here immediately. Each line therefore prints the
 * collating result NEXT TO both the ordinal and the linguistic answers.
 *
 * Only the sign is compared (the magnitude of a CRT compare is unspecified).
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static int sgn(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

int main(void) {
    static const struct { const wchar_t *a, *b; } t[] = {
        { L"Hello",   L"hello"   },   /* case only: ordinal-insensitive == equal   */
        { L"abc",     L"abd"     },
        { L"readme",  L"read-me" },   /* linguistic says -1, ordinal says +1       */
        { L"~",       L"a"       },   /* linguistic says -1, ordinal says +1       */
        { L"O'Brien", L"OBrien"  },   /* linguistic says +1, ordinal says -1       */
        { L"Z",       L"a"       },
        { L"a",       L"B"       },
        { L"",        L""        },
        { L"",        L"a"       },
        { L"file2",   L"file10"  },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        printf("[%ls|%ls] icoll=%d icmp=%d coll=%d cmp=%d lingi=%d\n",
               t[i].a, t[i].b,
               sgn(_wcsicoll(t[i].a, t[i].b)),
               sgn(_wcsicmp (t[i].a, t[i].b)),
               sgn(wcscoll  (t[i].a, t[i].b)),
               sgn(wcscmp   (t[i].a, t[i].b)),
               sgn(lstrcmpiW(t[i].a, t[i].b)));
    }
    return 0;
}
