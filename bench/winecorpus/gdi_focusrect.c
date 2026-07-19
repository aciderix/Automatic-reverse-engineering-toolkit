/* DrawFocusRect into a 32bpp memory DIB — the first native-control paint primitive
 * (a focus indicator; on FishTank.exe's import list). It is a 1px dotted outline,
 * XOR-inverted, so it is THEME-INDEPENDENT (pure XOR of the existing pixels — no system
 * color involved), hence bit-exact vs Wine regardless of the running theme. Oracle = an
 * FNV hash of the whole DIB + a few sampled pixels, bit-compared to Wine. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    const int W = 40, H = 20;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits;
    for (int i = 0; i < W * H; i++) px[i] = 0x00C0C0C0;   /* classic gray background */

    RECT r1 = { 2, 2, 22, 14 };
    DrawFocusRect(dc, &r1);
    RECT r2 = { 25, 4, 38, 16 };   /* a second rect at odd offset, to test parity tiling */
    DrawFocusRect(dc, &r2);
    GdiFlush();

    unsigned h = 2166136261u;
    for (int i = 0; i < W * H; i++) { unsigned c = px[i] & 0xFFFFFF;
        h ^= (c & 0xFF); h *= 16777619u; h ^= ((c >> 8) & 0xFF); h *= 16777619u; h ^= ((c >> 16) & 0xFF); h *= 16777619u; }
    /* sample the top edge of r1 (y=2): x=2 gap(4 even), x=3 dot(5 odd) */
    printf("r1 top: x2=%06x x3=%06x x4=%06x | left x2y3=%06x\n",
           px[2 * W + 2] & 0xFFFFFF, px[2 * W + 3] & 0xFFFFFF, px[2 * W + 4] & 0xFFFFFF, px[3 * W + 2] & 0xFFFFFF);
    printf("focus_dibhash=%08x\n", h);
    printf("done\n");
    return 0;
}
