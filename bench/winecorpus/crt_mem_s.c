/* memmove_s and memcpy_s side by side. WinMerge/MFC calls memmove_s.
 *
 * They are tested TOGETHER because that is the only way to pin down what separates
 * them, and what separates them is surprising: on a failure memcpy_s ZEROES the first
 * destsz bytes of the destination, while memmove_s leaves the destination COMPLETELY
 * UNTOUCHED. Everything else about the two matches, so implementing one from the other
 * is the obvious move — and it would silently wipe a caller's buffer that the real
 * msvcrt leaves alone. Only the raw bytes show it; both return the same code.
 *
 * count == 0 is the other trap: it short-circuits ALL validation and succeeds, even
 * with a NULL destination, on both functions. A shim validating first would return
 * EINVAL where msvcrt returns success.
 *
 * The overlap case is memmove's whole reason to exist and is checked explicitly.
 *
 * The companion crt_mem_s.def forces both names to be IMPORTED from msvcrt. Without it
 * mingw links its OWN bodies for these two — the exe then imports only memcpy/memmove/
 * memset — and the fixture would measure mingw on both sides rather than the oracle,
 * gating nothing. Confirmed with objdump before adding the .def, not assumed.
 *
 * Expected identical under Wine and ARET. */
#include <stdio.h>
#include <string.h>

typedef int errno_t;
errno_t __cdecl memmove_s(void *, size_t, const void *, size_t);
errno_t __cdecl memcpy_s(void *, size_t, const void *, size_t);

static void dump(const char *tag, int r, const unsigned char *b)
{
    printf("%-26s r=%2d raw=", tag, r);
    for (int i = 0; i < 8; i++) printf("%02x", b[i]);
    puts("");
}

int main(void)
{
    unsigned char b[8];
    int r;
    const char *s = "ABCDE";
#define R(tag, call) do { memset(b, 0xAB, 8); r = (call); dump(tag, r, b); } while (0)

    R("mv nominal(8,5)",   memmove_s(b, 8, s, 5));
    R("cp nominal(8,5)",   memcpy_s(b, 8, s, 5));
    R("mv exact(5,5)",     memmove_s(b, 5, s, 5));
    R("cp exact(5,5)",     memcpy_s(b, 5, s, 5));
    /* the asymmetry: same code, opposite destination state */
    R("mv too small(3,5)", memmove_s(b, 3, s, 5));
    R("cp too small(3,5)", memcpy_s(b, 3, s, 5));
    R("mv src NULL",       memmove_s(b, 8, NULL, 5));
    R("cp src NULL",       memcpy_s(b, 8, NULL, 5));
    R("mv size=0",         memmove_s(b, 0, s, 5));
    R("cp size=0",         memcpy_s(b, 0, s, 5));
    /* count 0 succeeds without validating anything */
    R("mv count=0",        memmove_s(b, 8, s, 0));
    R("cp count=0",        memcpy_s(b, 8, s, 0));
    printf("mv dst NULL n=5 r=%d n=0 r=%d\n",
           memmove_s(NULL, 8, s, 5), memmove_s(NULL, 8, s, 0));
    printf("cp dst NULL n=5 r=%d n=0 r=%d\n",
           memcpy_s(NULL, 8, s, 5), memcpy_s(NULL, 8, s, 0));

    /* Overlap, forwards and backwards — what memmove exists for. */
    memset(b, 0xAB, 8); memcpy(b, "ABCDE", 5);
    r = memmove_s(b + 1, 7, b, 4);
    dump("mv overlap fwd", r, b);
    memset(b, 0xAB, 8); memcpy(b, "ABCDE", 5);
    r = memmove_s(b, 8, b + 1, 4);
    dump("mv overlap back", r, b);
    return 0;
}
