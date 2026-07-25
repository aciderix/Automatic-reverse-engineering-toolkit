/* Palette family on a truecolor (32bpp) DC. Windows palettes do no colour remapping on
   a >8bpp target, but the object model and queries must still behave exactly. We create a
   logical palette, round-trip its entries, query the default system palette (which a
   truecolor DC reports as 0 entries yet fills with the 20 static reserved colours),
   resize, and take nearest-index/nearest-colour — all measured bit-for-bit against Wine so
   our HLE palette shims match. Deterministic (no pixels), runs under the harness. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = -8;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = 0; HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, bmp);

    char buf[sizeof(LOGPALETTE) + 3 * sizeof(PALETTEENTRY)];
    LOGPALETTE *lp = (LOGPALETTE *)buf; lp->palVersion = 0x300; lp->palNumEntries = 3;
    lp->palPalEntry[0] = (PALETTEENTRY){ 10, 20, 30, 0 };
    lp->palPalEntry[1] = (PALETTEENTRY){ 40, 50, 60, 0 };
    lp->palPalEntry[2] = (PALETTEENTRY){ 70, 80, 90, 0 };
    HPALETTE hp = CreatePalette(lp);
    printf("CreatePalette nonnull=%d\n", hp != NULL);

    HPALETTE old = SelectPalette(dc, hp, FALSE);
    printf("SelectPalette oldnonnull=%d\n", old != NULL);
    printf("RealizePalette=%u\n", RealizePalette(dc));
    printf("GetSystemPaletteUse=%u\n", GetSystemPaletteUse(dc));

    PALETTEENTRY sp[16]; memset(sp, 0xAA, sizeof sp);
    UINT n = GetSystemPaletteEntries(dc, 0, 16, sp);
    printf("SysPalEntries n=%u\n", n);
    for (int i = 0; i < 16; i++)
        printf("  [%d] %d,%d,%d,%d\n", i, sp[i].peRed, sp[i].peGreen, sp[i].peBlue, sp[i].peFlags);
    PALETTEENTRY hi[10]; memset(hi, 0xAA, sizeof hi);
    GetSystemPaletteEntries(dc, 246, 10, hi);
    for (int i = 0; i < 10; i++)
        printf("  [%d] %d,%d,%d\n", 246 + i, hi[i].peRed, hi[i].peGreen, hi[i].peBlue);

    PALETTEENTRY ge[3]; memset(ge, 0, sizeof ge);
    UINT gn = GetPaletteEntries(hp, 0, 3, ge);
    printf("GetPaletteEntries gn=%u e0=%d,%d,%d e2=%d,%d,%d\n", gn,
           ge[0].peRed, ge[0].peGreen, ge[0].peBlue, ge[2].peRed, ge[2].peGreen, ge[2].peBlue);
    printf("GetPaletteEntries count-query=%u\n", GetPaletteEntries(hp, 0, 0, NULL));

    printf("NearestIndex(41,51,61)=%d\n", GetNearestPaletteIndex(hp, RGB(41, 51, 61)));
    printf("NearestColor(10,20,30)=0x%06X\n", (unsigned)GetNearestColor(dc, RGB(10, 20, 30)));

    WORD cnt = 0; int go = GetObject(hp, sizeof(WORD), &cnt);
    printf("GetObject ret=%d count=%u\n", go, cnt);
    printf("ResizePalette=%d\n", ResizePalette(hp, 5));
    cnt = 0; GetObject(hp, sizeof(WORD), &cnt);
    printf("count-after-resize=%u\n", cnt);
    printf("UnrealizeObject=%d\n", UnrealizeObject(hp));

    SelectPalette(dc, (HPALETTE)GetStockObject(DEFAULT_PALETTE), FALSE);
    printf("DEFAULT_PALETTE entries=%u\n", GetPaletteEntries((HPALETTE)GetStockObject(DEFAULT_PALETTE), 0, 0, NULL));
    DeleteObject(hp);
    printf("done\n");
    return 0;
}
