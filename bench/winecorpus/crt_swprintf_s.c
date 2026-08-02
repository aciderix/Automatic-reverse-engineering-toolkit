/* swprintf_s / vswprintf_s — the secure wide sprintf with no separate count.
 *
 * The capacity is SWEPT (0..8 on a 5-character result) rather than sampled, because
 * everything interesting is on the failure side and the regimes are one apart:
 *   - capacity 0 leaves the buffer COMPLETELY untouched;
 *   - a capacity too small by any amount ZERO-FILLS exactly `capacity` units — not
 *     just dst[0], not the whole array;
 *   - an exact fit writes the text and its NUL and returns the LENGTH.
 * The buffer is poisoned and dumped raw, since a shim that wrote only a terminator
 * on failure would satisfy every caller that just reads a string back.
 *
 * ⚠️ These calls must really reach MSVCRT. mingw ships its own bodies for several
 * `*_s` functions, and a fixture that measures mingw on both sides guards nothing
 * (doc 70 section 7). Verified with objdump: `swprintf_s` (ordinal 1106) and
 * `vswprintf_s` (1137) appear in this binary's import table. That check is why the
 * narrow twins are NOT here — see below.
 *
 * ⚠️ `sprintf_s`/`vsprintf_s` are deliberately absent. Forced to msvcrt through a
 * .def, Wine's narrow versions behave differently from the wide ones: partial output
 * left on failure instead of a zero fill, and at an exact fit a success return with
 * NO terminator written. ARET leaves them unimplemented (loud abort) rather than
 * reproduce that from a single oracle; the question is queued for the Windows runner
 * (bench/winoracle/crt_sprintfs_disputed.c).
 *
 * Expected identical under Wine and ARET. */
#define MINGW_HAS_SECURE_API 1
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>
#include <string.h>

static int callv(WCHAR *b, size_t n, const WCHAR *f, ...)
{
    va_list ap;
    va_start(ap, f);
    int r = vswprintf_s(b, n, f, ap);
    va_end(ap);
    return r;
}

static void dump(const char *what, int cap, int r, const WCHAR *b, int n)
{
    printf("%-14s cap=%-2d r=%-3d [", what, cap, r);
    for (int i = 0; i < n; i++) printf("%04x", b[i]);
    printf("]\n");
}

int main(void)
{
    WCHAR w[16];

    for (int cap = 0; cap <= 8; cap++) {
        memset(w, 0xAA, sizeof w);
        int r = swprintf_s(w, cap, L"%d-%s", 42, L"xy");   /* "42-xy", 5 chars */
        dump("swprintf_s", cap, r, w, 8);
    }
    for (int cap = 0; cap <= 8; cap++) {
        memset(w, 0xAA, sizeof w);
        int r = callv(w, cap, L"%d-%s", 42, L"xy");
        dump("vswprintf_s", cap, r, w, 8);
    }

    /* An empty result still needs its terminator, and must not touch more. */
    memset(w, 0xAA, sizeof w);
    { int r = swprintf_s(w, 4, L"");
      printf("empty  r=%d u0=%04x u1=%04x\n", r, w[0], w[1]); }

    /* Exact fit: the NUL lands on the last slot and nothing beyond moves. */
    memset(w, 0xAA, sizeof w);
    { int r = swprintf_s(w, 6, L"%d-%s", 42, L"xy");
      printf("exact  r=%d u5=%04x u6=%04x\n", r, w[5], w[6]); }

    /* A few format shapes, so this also guards the wide formatter itself. */
    memset(w, 0xAA, sizeof w);
    { int r = swprintf_s(w, 16, L"[%5d][%-4s][%c]", -7, L"ab", 'Z');
      printf("shapes r=%d \"%ls\"\n", r, w); }
    memset(w, 0xAA, sizeof w);
    { int r = swprintf_s(w, 16, L"%x %o %u", 0xbeef, 8, 4000000000u);
      printf("radix  r=%d \"%ls\"\n", r, w); }
    return 0;
}
