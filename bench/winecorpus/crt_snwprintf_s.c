/* _snwprintf_s — the secure wide snprintf. WinMerge/MFC calls it during startup.
 *
 * The cases here exist to separate FOUR regimes that a single "truncate and return -1"
 * implementation would blur into one, and that return values alone cannot tell apart:
 * a result that fits returns its length; a result limited by `count` keeps what it
 * wrote and returns -1; a result truncated because the caller passed _TRUNCATE keeps
 * what it wrote and returns -1; but a result too long for the BUFFER — truncation the
 * caller never asked for — ZEROES the whole buffer, every element and not just the
 * first, and returns -1. Only the raw bytes distinguish the last two, so they are
 * printed rather than read back as a string.
 *
 * Three edges are measured too, and each is a different answer: size 0 leaves the
 * buffer completely untouched, a NULL format empties it, and a NULL BUFFER is a length
 * QUERY that succeeds rather than an error.
 *
 * Expected identical under Wine and ARET. */
#include <stdio.h>
#include <wchar.h>

int _snwprintf_s(wchar_t *, size_t, size_t, const wchar_t *, ...);
#define TRUNC ((size_t)-1)

static void dump(const char *tag, int r, const wchar_t *b)
{
    printf("%-28s r=%3d raw=", tag, r);
    for (int i = 0; i < 8; i++) printf("%04x", (unsigned)b[i]);
    puts("");
}

int main(void)
{
    wchar_t w[8];
    int r;
#define R(tag, call) do { for (int i = 0; i < 8; i++) w[i] = 0x2020; \
                          r = (call); dump(tag, r, w); } while (0)

    R("fits(size8,count7)",      _snwprintf_s(w, 8, 7, L"ab%d", 42));
    R("exact(size5,count4)",     _snwprintf_s(w, 5, 4, L"ab%d", 42));
    /* count is the binding limit, buffer had room: keeps what it wrote */
    R("count limits(size8,c2)",  _snwprintf_s(w, 8, 2, L"abcdef"));
    R("count=0",                 _snwprintf_s(w, 8, 0, L"abcdef"));
    /* buffer too small and truncation NOT requested: whole buffer zeroed */
    R("buffer too small(4,c9)",  _snwprintf_s(w, 4, 9, L"abcdef"));
    /* truncation requested: keeps size-1 characters */
    R("_TRUNCATE size4",         _snwprintf_s(w, 4, TRUNC, L"abcdef"));
    R("_TRUNCATE size1",         _snwprintf_s(w, 1, TRUNC, L"abcdef"));
    R("_TRUNCATE fits",          _snwprintf_s(w, 8, TRUNC, L"abc"));
    R("empty format",            _snwprintf_s(w, 8, 7, L""));
    R("width/precision",         _snwprintf_s(w, 8, 7, L"%5.2f", 3.14159));
    R("string arg",              _snwprintf_s(w, 8, 7, L"[%ls]", L"hi"));

    /* size 0: untouched, unlike every other failure here */
    for (int i = 0; i < 8; i++) w[i] = 0x2020;
    r = _snwprintf_s(w, 0, 7, L"abc");
    printf("size=0 r=%d w0=%04x (untouched)\n", r, (unsigned)w[0]);
    /* NULL buffer: a length query, not an error */
    printf("NULL buffer r=%d\n", _snwprintf_s(NULL, 8, 7, L"abc"));
    printf("NULL buffer size0 r=%d\n", _snwprintf_s(NULL, 0, 7, L"abc"));
    return 0;
}
