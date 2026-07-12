/* GDI Polyline (G6+, doc 72), bit-identical to Wine: count-1 connected Bresenham
 * segments with the selected pen; each segment excludes its endpoint (shared
 * vertices drawn by the next segment's start, final endpoint not drawn). DIB hash
 * oracle. Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    const int W = 24, H = 20;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;

    POINT box[4] = { {2, 2}, {21, 2}, {21, 17}, {2, 17} };
    Polyline(dc, box, 4);                          /* open box outline */
    HPEN rp = CreatePen(PS_SOLID, 1, RGB(0, 0, 200));
    SelectObject(dc, rp);
    POINT zig[5] = { {3, 4}, {8, 15}, {13, 4}, {18, 15}, {21, 8} };
    Polyline(dc, zig, 5);                           /* zig-zag, coloured */
    GdiFlush();

    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    printf("polyline=%08x\n", h);
    printf("done\n");
    return 0;
}
