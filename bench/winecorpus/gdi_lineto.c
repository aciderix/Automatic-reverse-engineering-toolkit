/* GDI vector lines (G6+, doc 72): MoveToEx / LineTo, bit-identical to Wine. LineTo
 * draws an integer Bresenham line from the current position to (x,y) with the
 * selected pen, excluding the endpoint (GDI semantics). Covers all octants. The
 * DIB buffer is hashed → winediff compares exactly. Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    const int W = 24, H = 24;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;

    /* default BLACK_PEN, then a coloured pen for one line */
    MoveToEx(dc, 2, 2, NULL);  LineTo(dc, 22, 10);   /* shallow +x major */
    MoveToEx(dc, 2, 2, NULL);  LineTo(dc, 10, 22);   /* steep +y major  */
    MoveToEx(dc, 22, 2, NULL); LineTo(dc, 2, 20);    /* negative dx     */
    MoveToEx(dc, 12, 2, NULL); LineTo(dc, 12, 22);   /* vertical        */
    MoveToEx(dc, 2, 12, NULL); LineTo(dc, 22, 12);   /* horizontal      */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xE0, 0x20, 0x40));
    SelectObject(dc, pen);
    MoveToEx(dc, 2, 22, NULL); LineTo(dc, 22, 2);    /* coloured diagonal */
    GdiFlush();

    POINT cp; GetCurrentPositionEx(dc, &cp);
    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    printf("lines hash=%08x curpos=%ld,%ld\n", h, cp.x, cp.y);
    printf("done\n");
    return 0;
}
