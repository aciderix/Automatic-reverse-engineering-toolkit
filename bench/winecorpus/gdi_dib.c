/* Exercises the GDI object/DC model + bit-exact DIB drawing (G6, doc 72). All
 * drawing targets a 32bpp memory DIB section (a buffer we own), so FillRect /
 * SetPixel / PatBlt are exact memory writes that match Wine's DIB byte-for-byte.
 * The oracle hashes the buffer + reads pixels back (exact), and checks the object
 * model by invariant (stock handles distinct/non-null; device caps class). No
 * text/pen-edged shapes (not bit-reproducible vs Wine's rasteriser). Needs a
 * display for Wine's GDI init (Xvfb), like the window/dialog fixtures. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = -8;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    printf("dib ok=%d\n", hbm != NULL && bits != NULL);
    SelectObject(dc, hbm);

    HBRUSH bg = CreateSolidBrush(RGB(0x11, 0x22, 0x33));
    RECT full = {0, 0, 8, 8}; FillRect(dc, &full, bg);
    HBRUSH fg = CreateSolidBrush(RGB(0xF0, 0x0D, 0x42));
    RECT sub = {2, 2, 6, 6}; FillRect(dc, &sub, fg);

    SetPixel(dc, 0, 0, RGB(0xAB, 0xCD, 0xEF));
    SetPixel(dc, 7, 7, RGB(1, 2, 3));
    SetPixelV(dc, 7, 0, RGB(9, 8, 7));
    PatBlt(dc, 0, 6, 2, 2, BLACKNESS);
    SelectObject(dc, fg);
    PatBlt(dc, 6, 0, 2, 2, PATCOPY);
    GdiFlush();

    unsigned char *p = (unsigned char *)bits; unsigned h = 2166136261u;
    for (int i = 0; i < 8 * 8 * 4; i++) { h ^= p[i]; h *= 16777619u; }
    printf("hash=%08x\n", h);
    printf("pix00=%06x pix77=%06x pix33=%06x\n",
           (unsigned)GetPixel(dc, 0, 0), (unsigned)GetPixel(dc, 7, 7), (unsigned)GetPixel(dc, 3, 3));

    HGDIOBJ wb = GetStockObject(WHITE_BRUSH), bb = GetStockObject(BLACK_BRUSH);
    printf("stock wb=%d bb=%d distinct=%d\n", wb != NULL, bb != NULL, wb != bb);

    HDC sdc = GetDC(NULL);
    printf("caps tech=%d bpp>=8=%d horz>0=%d\n",
           GetDeviceCaps(sdc, TECHNOLOGY), GetDeviceCaps(sdc, BITSPIXEL) >= 8, GetDeviceCaps(sdc, HORZRES) > 0);
    ReleaseDC(NULL, sdc);

    DeleteObject(bg); DeleteObject(fg); DeleteObject(hbm); DeleteDC(dc);
    printf("done\n");
    return 0;
}
