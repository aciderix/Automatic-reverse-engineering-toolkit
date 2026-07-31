/* strcpy_s — bounded copy (msvcrt secure CRT). WinMerge/MFC calls it.
 *
 * Statically imported, and that resolves to msvcrt's ordinal 1084 rather than a mingw
 * body — worth checking rather than assuming, because mingw does supply its own
 * implementation for some secure-CRT names, and measuring that instead of the oracle
 * would silently calibrate against the wrong thing. (Here the two were compared and
 * agree, but the check is the point.)
 *
 * Only the narrow variant is gated: mingw's import library does not expose wcscpy_s, so
 * it cannot be linked into a fixture. wcscpy_s is implemented from Wine measurements
 * taken through GetProcAddress and is NOT covered here — see 70 §P1quater, which is also
 * why routing this fixture through GetProcAddress would not work.
 *
 * Poisoned buffer, raw bytes printed: the failure modes differ in exactly which bytes
 * move, and are indistinguishable from return values alone. In particular the too-small
 * case has already copied the characters that fitted before emptying the string, which a
 * "nothing was written on failure" implementation would get wrong.
 *
 * Expected identical under Wine and ARET. */
#include <stdio.h>
#include <string.h>

typedef int errno_t;
errno_t __cdecl strcpy_s(char *, size_t, const char *);

static void show(const char *tag, errno_t r, const char *b)
{
    printf("%-26s r=%d raw=[", tag, r);
    for (int i = 0; i < 8; i++) printf("%02x", (unsigned char)b[i]);
    printf("]\n");
}

int main(void)
{
    char b[8];
    int r;
    /* The call is sequenced BEFORE the buffer is read, deliberately: passing both the
     * call and b[i] as printf arguments leaves their evaluation order unspecified, and
     * reading the buffer before the call silently reports pre-call bytes. */
    memset(b, 0xAB, 8); r = strcpy_s(b, 8, "MiXeD"); show("nominal(5->8)", r, b);
    memset(b, 0xAB, 8); r = strcpy_s(b, 6, "MiXeD"); show("exact(5->6)", r, b);
    /* too small: what fitted is copied, then the string is emptied */
    memset(b, 0xAB, 8); r = strcpy_s(b, 5, "MiXeD"); show("too small(5->5)", r, b);
    memset(b, 0xAB, 8); r = strcpy_s(b, 3, "MiXeD"); show("too small(5->3)", r, b);
    memset(b, 0xAB, 8); r = strcpy_s(b, 1, "MiXeD"); show("too small(5->1)", r, b);
    /* size 0: untouched, unlike every other failure here */
    memset(b, 0xAB, 8); r = strcpy_s(b, 0, "MiXeD"); show("size=0", r, b);
    memset(b, 0xAB, 8); r = strcpy_s(b, 8, "");      show("src empty", r, b);
    /* src NULL: destination emptied */
    memset(b, 0xAB, 8); r = strcpy_s(b, 8, NULL);    show("src NULL", r, b);
    memset(b, 0xAB, 8); r = strcpy_s(b, 0, NULL);    show("src NULL size=0", r, b);
    printf("dst NULL r=%d\n", strcpy_s(NULL, 8, "x"));
    printf("dst NULL size=0 r=%d\n", strcpy_s(NULL, 0, "x"));
    return 0;
}
