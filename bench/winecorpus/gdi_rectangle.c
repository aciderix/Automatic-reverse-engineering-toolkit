/* GDI Rectangle (G6+, doc 72), bit-identical to Wine: interior [l+1,r-1)×[t+1,b-1)
 * filled with the selected brush, outline [l,r-1]×[t,b-1] drawn with the selected
 * pen (1px). Covers filled, hollow (NULL_BRUSH), and coloured-pen cases. DIB hash
 * oracle. Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    const int W = 30, H = 24;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;

    HBRUSH g = CreateSolidBrush(RGB(0, 200, 0));
    SelectObject(dc, g);
    Rectangle(dc, 2, 2, 14, 10);                       /* green fill, black pen */

    HPEN rp = CreatePen(PS_SOLID, 1, RGB(200, 0, 0));
    SelectObject(dc, rp);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 16, 4, 28, 20);                      /* hollow, red border */

    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    SelectObject(dc, GetStockObject(BLACK_PEN));
    Rectangle(dc, 4, 14, 12, 22);                      /* white fill, black pen */
    GdiFlush();

    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    printf("rects=%08x\n", h);
    printf("done\n");
    return 0;
}
