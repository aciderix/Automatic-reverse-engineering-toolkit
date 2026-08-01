/* strnlen / wcsnlen — bounded string length. WinMerge/MFC calls wcsnlen.
 *
 * crt_nlen.def forces both to be IMPORTED from msvcrt: mingw links its own bodies
 * otherwise, and the fixture would then measure mingw on both sides and gate nothing.
 * Checked with objdump rather than assumed — the same trap crt_mem_s fell into.
 *
 * The case that matters is a buffer with NO terminator inside the window. That is what
 * the bounded form exists for, and what a "strlen then clamp" implementation gets
 * wrong: it would run off the end of the very buffer these functions are meant to make
 * safe. It is exercised here with a deliberately unterminated array, so a wrong
 * implementation crashes or reads garbage rather than quietly agreeing.
 *
 * Expected identical under Wine and ARET. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

size_t __cdecl wcsnlen(const wchar_t *, size_t);
size_t __cdecl strnlen(const char *, size_t);

int main(void)
{
    printf("w max8=%u max5=%u max3=%u max1=%u max0=%u\n",
           (unsigned)wcsnlen(L"abcde", 8), (unsigned)wcsnlen(L"abcde", 5),
           (unsigned)wcsnlen(L"abcde", 3), (unsigned)wcsnlen(L"abcde", 1),
           (unsigned)wcsnlen(L"abcde", 0));
    printf("a max8=%u max5=%u max3=%u max1=%u max0=%u\n",
           (unsigned)strnlen("abcde", 8), (unsigned)strnlen("abcde", 5),
           (unsigned)strnlen("abcde", 3), (unsigned)strnlen("abcde", 1),
           (unsigned)strnlen("abcde", 0));
    printf("empty w=%u a=%u\n", (unsigned)wcsnlen(L"", 8), (unsigned)strnlen("", 8));

    /* No terminator within maxlen: must stop at maxlen without reading past it. */
    wchar_t wnz[4] = { 'a', 'b', 'c', 'd' };
    char anz[4] = { 'a', 'b', 'c', 'd' };
    printf("unterminated w=%u a=%u\n",
           (unsigned)wcsnlen(wnz, 4), (unsigned)strnlen(anz, 4));
    /* A window shorter than the array, still unterminated. */
    printf("unterminated short w=%u a=%u\n",
           (unsigned)wcsnlen(wnz, 2), (unsigned)strnlen(anz, 2));
    /* maxlen 0 answers 0 and reads nothing at all, NULL included. */
    printf("null max0 w=%u a=%u\n",
           (unsigned)wcsnlen(NULL, 0), (unsigned)strnlen(NULL, 0));
    return 0;
}
