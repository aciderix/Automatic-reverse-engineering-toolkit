/* GDI underline / strikeout (G3, doc 72): CreateFont lfUnderline / lfStrikeOut,
 * bit-identical to Wine. Each is a solid text-colour bar spanning the text extent;
 * positions match Wine's OUTLINETEXTMETRIC (underline from the post table's
 * position+thickness/2, strikeout from OS/2 yStrikeoutPosition), thicknesses from
 * the scaled font metrics (min 1px). Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static unsigned draw(int underline, int strikeout, int h) {
    const int W = 70, H = 44;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;
    HFONT f = CreateFontA(h, 0, 0, 0, FW_NORMAL, 0, underline, strikeout, ANSI_CHARSET,
        0, 0, NONANTIALIASED_QUALITY, 0, "DejaVu Sans");
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0)); SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, 2, 3, "Text", 4); GdiFlush();
    unsigned hh = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { hh ^= p[i]; hh *= 16777619u; }
    DeleteObject(f); DeleteObject(hbm); DeleteDC(dc);
    return hh;
}

int main(void) {
    printf("ul16=%08x so16=%08x both24=%08x ul32=%08x\n",
        draw(1, 0, -16), draw(0, 1, -16), draw(1, 1, -24), draw(1, 0, -32));
    printf("done\n");
    return 0;
}
