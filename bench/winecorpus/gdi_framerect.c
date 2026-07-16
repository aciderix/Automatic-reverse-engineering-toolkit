/* GDI FrameRect / InvertRect / PolylineTo (G6+, doc 72), bit-identical to Wine:
 *  - FrameRect: 1px border on the outline [l,r-1]x[t,b-1] with the *argument*
 *    brush's colour (measured on Wine).
 *  - InvertRect: XOR every pixel (all 32 bits) over [l,r)x[t,b).
 *  - PolylineTo: a run of LineTo from the current position (endpoint excluded
 *    per segment), updating the current position to the last point.
 * DIB hash oracle. Needs a display (Xvfb). */
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
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00112233;

    HBRUSH g = CreateSolidBrush(RGB(0, 200, 0));
    RECT fr = {2, 2, 14, 10};
    FrameRect(dc, &fr, g);                              /* green 1px border */

    RECT ir = {16, 3, 27, 11};
    InvertRect(dc, &ir);                                /* XOR block */

    HPEN bp = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    SelectObject(dc, bp);
    MoveToEx(dc, 3, 14, NULL);
    POINT pts[3] = {{26, 14}, {26, 21}, {3, 21}};
    PolylineTo(dc, pts, 3);                             /* connected polyline, cur-tracked */
    POINT cur; GetCurrentPositionEx(dc, &cur);
    GdiFlush();

    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    printf("frame_invert_poly=%08x\n", h);
    printf("cur=%d,%d\n", (int)cur.x, (int)cur.y);
    printf("done\n");
    return 0;
}
