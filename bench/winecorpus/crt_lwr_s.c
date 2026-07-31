/* _strlwr_s — in-place lowercase with a buffer-size check (msvcrt secure CRT). This is
 * the import WinMerge/MFC stops on.
 *
 * Only the narrow lowercase variant is exercised here, and that is a real limitation
 * worth stating: mingw's import library exposes _strlwr_s but NOT _strupr_s, _wcslwr_s
 * or _wcsupr_s, so those three cannot be linked into a fixture at all. They are
 * implemented from measurements taken through GetProcAddress under Wine, but they are
 * not covered by this gate. (Routing the fixture through GetProcAddress instead would
 * not help: ARET's GetProcAddress currently always answers NULL, so the fixture would
 * exit early on the ARET side and compare nothing.)
 *
 * Every call runs against a POISONED buffer whose raw bytes are printed, because which
 * bytes move IS the contract. Two failure modes look alike and are not: with size 0 the
 * narrow functions leave the buffer completely alone, whereas a size that is merely too
 * small EMPTIES the string (str[0] = 0) and keeps the remaining bytes. Both return the
 * same code, so a test reading only return values could not tell them apart.
 *
 * Expected identical under Wine and ARET. */
#include <stdio.h>
#include <string.h>

typedef int errno_t;
errno_t __cdecl _strlwr_s(char *, size_t);

static void show(const char *tag, errno_t r, const char *b, size_t n)
{
    printf("%-24s r=%d raw=[", tag, r);
    for (size_t i = 0; i < n; i++) printf("%02x", (unsigned char)b[i]);
    printf("]\n");
}

int main(void)
{
    char b[8];
    memset(b, 0xAB, 8); strcpy(b, "MiXeD"); show("nominal",        _strlwr_s(b, 8), b, 8);
    memset(b, 0xAB, 8); strcpy(b, "MiXeD"); show("size=len+1",     _strlwr_s(b, 6), b, 8);
    /* too small: string emptied, remainder of the buffer preserved */
    memset(b, 0xAB, 8); strcpy(b, "MiXeD"); show("size=len",       _strlwr_s(b, 5), b, 8);
    /* size 0: buffer untouched — the distinction this fixture exists for */
    memset(b, 0xAB, 8); strcpy(b, "MiXeD"); show("size=0",         _strlwr_s(b, 0), b, 8);
    memset(b, 0xAB, 8); strcpy(b, "MiXeD"); show("size=1",         _strlwr_s(b, 1), b, 8);
    memset(b, 0xAB, 8); b[0] = 0;           show("empty string",   _strlwr_s(b, 8), b, 8);
    /* non-letters pass through untouched; only A-Z move */
    memset(b, 0xAB, 8); strcpy(b, "A1_z%"); show("non-alpha",      _strlwr_s(b, 8), b, 8);
    memset(b, 0xAB, 8); strcpy(b, "aBcDe"); show("already-lower",  _strlwr_s(b, 8), b, 8);
    printf("NULL r=%d\n", _strlwr_s(NULL, 8));
    printf("NULL size=0 r=%d\n", _strlwr_s(NULL, 0));
    return 0;
}
