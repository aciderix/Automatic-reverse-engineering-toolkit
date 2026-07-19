/* DrawFrameControl (push button) into a 32bpp DIB. A normal DFC_BUTTON/DFCS_BUTTONPUSH
 * is a SOFT-raised bevel filled with 3DFACE (measured vs Wine). Verified structurally
 * (index-based, theme-independent) like gdi_drawedge: each ring pixel must equal the
 * system color of the index the frame uses there, so ARET's classic palette and Wine's
 * theme both print the same 1s. */
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
    for (int i = 0; i < W * H; i++) px[i] = 0x00C0C0C0;
#define AT(x, y) (px[(y) * W + (x)] & 0xFFFFFF)
#define SC(i)    (GetSysColor(i) & 0xFFFFFF)

    RECT r = { 2, 2, 20, 18 };
    BOOL ok = DrawFrameControl(dc, &r, DFC_BUTTON, DFCS_BUTTONPUSH);
    /* soft raised: outer TL=BTNHIGHLIGHT, outer BR=3DDKSHADOW, inner TL=3DLIGHT,
     * inner BR=BTNSHADOW, face=3DFACE */
    printf("push ok=%d oTL=%d oBR=%d iTL=%d iBR=%d face=%d\n", ok,
           AT(2, 2)   == SC(COLOR_BTNHIGHLIGHT),
           AT(19, 17) == SC(COLOR_3DDKSHADOW),
           AT(3, 3)   == SC(COLOR_3DLIGHT),
           AT(18, 16) == SC(COLOR_BTNSHADOW),
           AT(10, 10) == SC(COLOR_3DFACE));
    printf("done\n");
    return 0;
}
