/* Locale A-twins: CompareStringA/W, LCMapStringA (upper/lower), GetStringTypeA.
 * Ordinal-ish/case for ASCII/en-US, matches Wine (deep collation not modelled). */
#include <windows.h>
#include <stdio.h>
int main(void) {
    printf("cmp_ab=%d cmp_ba=%d cmp_eq=%d cmp_ci=%d\n",
        CompareStringA(0x409, 0, "apple", -1, "banana", -1),
        CompareStringA(0x409, 0, "banana", -1, "apple", -1),
        CompareStringA(0x409, 0, "same", -1, "same", -1),
        CompareStringA(0x409, NORM_IGNORECASE, "HELLO", -1, "hello", -1));
    char up[16], lo[16];
    int nu = LCMapStringA(0x409, LCMAP_UPPERCASE, "MixedCase", -1, up, sizeof up);
    int nl = LCMapStringA(0x409, LCMAP_LOWERCASE, "MixedCase", -1, lo, sizeof lo);
    printf("upper n=%d [%s] lower n=%d [%s]\n", nu, up, nl, lo);
    WORD ty[4]; GetStringTypeA(0x409, CT_CTYPE1, "A1 x", 4, ty);
    printf("ctype A=%04x 1=%04x sp=%04x x=%04x\n", ty[0], ty[1], ty[2], ty[3]);
    printf("done\n");
    return 0;
}
