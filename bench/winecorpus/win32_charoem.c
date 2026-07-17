/* CharToOemA / OemToCharA — ANSI(CP1252) <-> OEM(CP437) translation. Windows uses a
 * best-fit mapping here (not strict): a CP1252-only char with a near CP437 form maps
 * to it (U+201A -> ','), one with none maps to '?', a char present in both maps
 * directly (é 0xE9 -> 0x82). ARET's tables are extracted verbatim from Wine, so the
 * whole byte range round-trips bit-identically. Expected identical under Wine/ARET. */
#include <windows.h>
#include <stdio.h>
int main(void) {
    const char *s = "Hello \x80\x82\x92\xE9\xFF World";
    char oem[64]; memset(oem, 0, sizeof oem);
    int r = CharToOemA(s, oem) != 0;
    printf("r=%d oem=", r);
    for (int i = 0; oem[i]; i++) printf("%02x", (unsigned char)oem[i]);
    printf("\n");
    /* full-range round trip signature */
    unsigned sum = 0;
    for (int i = 1; i < 256; i++) {
        char in[2] = { (char)i, 0 }, a[2] = { 0, 0 }, b[2] = { 0, 0 };
        CharToOemA(in, a); OemToCharA(in, b);
        sum = sum * 131 + (unsigned char)a[0];
        sum = sum * 131 + (unsigned char)b[0];
    }
    printf("sig=%08x\n", sum);
    return 0;
}
