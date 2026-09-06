/* G6 raster wall (doc 72) — pen-edged SHAPES into a 32bpp memory DIB: LineTo and
 * Rectangle. Unlike gdi_dib.c (FillRect/PatBlt/SetPixel = exact memory writes), these
 * go through GDI's line/edge rasteriser, which ARET currently aborts on ("raster != Wine").
 * This fixture REPRODUCES and MEASURES that wall: it draws a horizontal line, a vertical
 * line, a diagonal, and a rectangle border, then hashes the DIB and reads back pixels on
 * each primitive's path. The oracle (winediff vs Wine, byte-exact) tells us precisely how
 * far ARET's raster is from Wine's — abort, or which pixels differ. Text is NOT here yet
 * (font raster is a separate, harder wall). Needs a display for Wine's GDI init (Xvfb). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 16; bi.bmiHeader.biHeight = -16;   /* top-down 16x16 */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    printf("dib ok=%d\n", hbm != NULL && bits != NULL);
    SelectObject(dc, hbm);

    /* Clear to a known background (exact FillRect, matches Wine per gdi_dib). */
    HBRUSH bg = CreateSolidBrush(RGB(0x00, 0x00, 0x00));
    RECT full = {0, 0, 16, 16}; FillRect(dc, &full, bg);

    /* Pen-edged shapes — the wall. */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xFF, 0x00, 0x00));
    HGDIOBJ oldpen = SelectObject(dc, pen);

    MoveToEx(dc, 1, 1, NULL); LineTo(dc, 14, 1);     /* horizontal */
    MoveToEx(dc, 1, 1, NULL); LineTo(dc, 1, 14);     /* vertical */
    MoveToEx(dc, 2, 2, NULL); LineTo(dc, 13, 13);    /* diagonal */

    HGDIOBJ oldbr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 4, 8, 12, 14);                      /* pen border, hollow */
    SelectObject(dc, oldbr);

    SelectObject(dc, oldpen);
    GdiFlush();

    unsigned char *p = (unsigned char *)bits; unsigned h = 2166136261u;
    for (int i = 0; i < 16 * 16 * 4; i++) { h ^= p[i]; h *= 16777619u; }
    printf("hash=%08x\n", h);
    /* Readbacks on each primitive's path (endpoints + midpoints). */
    printf("hline_1_1=%06x hline_14_1=%06x\n", (unsigned)GetPixel(dc, 1, 1), (unsigned)GetPixel(dc, 14, 1));
    printf("vline_1_14=%06x diag_7_7=%06x\n", (unsigned)GetPixel(dc, 1, 14), (unsigned)GetPixel(dc, 7, 7));
    printf("rect_tl_4_8=%06x rect_br_11_13=%06x\n", (unsigned)GetPixel(dc, 4, 8), (unsigned)GetPixel(dc, 11, 13));

    DeleteObject(pen); DeleteObject(bg); DeleteObject(hbm); DeleteDC(dc);
    printf("done\n");
    return 0;
}
