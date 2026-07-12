/* GDI text alignment (G3, doc 72): SetTextAlign origins, bit-identical to Wine.
 * Horizontal LEFT/RIGHT/CENTER shift the x origin by the text extent; vertical
 * TOP/BASELINE/BOTTOM shift the baseline. Each mode renders into a fresh 32bpp DIB
 * and the buffer is hashed → winediff compares exactly. Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static unsigned draw(unsigned align) {
    const int W = 80, H = 26;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;
    HFONT f = CreateFontA(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, "DejaVu Sans");
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0)); SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, align);
    TextOutA(dc, 40, 12, "Way", 3);
    GdiFlush();
    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    DeleteObject(f); DeleteObject(hbm); DeleteDC(dc);
    return h;
}

int main(void) {
    printf("left=%08x right=%08x center=%08x baseline=%08x bottom=%08x\n",
        draw(TA_LEFT | TA_TOP), draw(TA_RIGHT), draw(TA_CENTER),
        draw(TA_BASELINE), draw(TA_BOTTOM));
    printf("done\n");
    return 0;
}
