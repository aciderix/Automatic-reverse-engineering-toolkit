/* strncpy_s / wcsncpy_s — bounded copy of at most `count` characters (msvcrt secure
 * CRT). WinMerge/MFC calls wcsncpy_s.
 *
 * Both widths are exercised here, unlike the earlier secure-CRT fixtures, because
 * mingw's import library exposes both. That matters beyond convenience: the two
 * previous families each hid a narrow/wide asymmetry (size==0 in _strlwr_s, the
 * too-small case in strcpy_s), so "the widths agree" is a claim that has to be gated,
 * not assumed. Here they do agree, everywhere.
 *
 * Poisoned buffers with the raw bytes printed, and the call sequenced before the read
 * on its own statement — passing both to printf leaves evaluation order unspecified and
 * silently reports pre-call bytes.
 *
 * The interesting cases are the three that fail differently: a too-small destination
 * copies what fitted and THEN empties the string (ERANGE), _TRUNCATE truncates and
 * KEEPS the result (STRUNCATE 80, the only failure whose output is meant to be used),
 * and count 0 is not a failure at all — it succeeds with an empty destination.
 *
 * Expected identical under Wine and ARET. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef int errno_t;
errno_t __cdecl strncpy_s(char *, size_t, const char *, size_t);
errno_t __cdecl wcsncpy_s(wchar_t *, size_t, const wchar_t *, size_t);

#define TRUNCATE ((size_t)-1)

static void da(const char *tag, int r, const char *b)
{
    printf("A %-20s r=%d raw=", tag, r);
    for (int i = 0; i < 8; i++) printf("%02x", (unsigned char)b[i]);
    puts("");
}
static void dw(const char *tag, int r, const wchar_t *b)
{
    printf("W %-20s r=%d raw=", tag, r);
    for (int i = 0; i < 7; i++) printf("%04x", (unsigned)b[i]);
    puts("");
}

int main(void)
{
    char a[8];
    wchar_t w[8];
    int r;

#define RA(tag, call) do { memset(a, 0xAB, 8); r = (call); da(tag, r, a); } while (0)
#define RW(tag, call) do { for (int i = 0; i < 8; i++) w[i] = 0x2020; r = (call); dw(tag, r, w); } while (0)

    RA("count<len(3)",   strncpy_s(a, 8, "MiXeD", 3));
    RA("count=len(5)",   strncpy_s(a, 8, "MiXeD", 5));
    RA("count>len(9)",   strncpy_s(a, 8, "MiXeD", 9));
    RA("dest too small", strncpy_s(a, 4, "MiXeD", 5));
    RA("_TRUNCATE",      strncpy_s(a, 4, "MiXeD", TRUNCATE));
    RA("_TRUNCATE fits", strncpy_s(a, 8, "MiXeD", TRUNCATE));
    RA("count=0",        strncpy_s(a, 8, "MiXeD", 0));
    RA("size=0",         strncpy_s(a, 0, "MiXeD", 3));
    RA("src NULL",       strncpy_s(a, 8, NULL, 3));

    RW("count<len(3)",   wcsncpy_s(w, 8, L"MiXeD", 3));
    RW("count=len(5)",   wcsncpy_s(w, 8, L"MiXeD", 5));
    RW("count>len(9)",   wcsncpy_s(w, 8, L"MiXeD", 9));
    RW("dest too small", wcsncpy_s(w, 4, L"MiXeD", 5));
    RW("_TRUNCATE",      wcsncpy_s(w, 4, L"MiXeD", TRUNCATE));
    RW("_TRUNCATE fits", wcsncpy_s(w, 8, L"MiXeD", TRUNCATE));
    RW("count=0",        wcsncpy_s(w, 8, L"MiXeD", 0));
    RW("size=0",         wcsncpy_s(w, 0, L"MiXeD", 3));
    RW("src NULL",       wcsncpy_s(w, 8, NULL, 3));

    printf("dest NULL A r=%d W r=%d\n",
           strncpy_s(NULL, 8, "x", 1), wcsncpy_s(NULL, 8, L"x", 1));
    return 0;
}
