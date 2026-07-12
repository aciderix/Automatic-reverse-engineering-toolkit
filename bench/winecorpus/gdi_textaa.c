/* GDI antialiased text (G3, doc 72): DEFAULT_QUALITY / CLEARTYPE_QUALITY render
 * with FreeType LCD subpixel rendering (FT_LOAD_TARGET_LCD + the default LCD
 * filter) — the same path Wine uses when subpixel/ClearType smoothing is enabled
 * — so the RGB subpixel values match Wine bit-for-bit. Covers black, coloured, and
 * multi-glyph strings. (Grayscale ANTIALIASED_QUALITY is a distinct Wine pipeline,
 * still a sound abort.) Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static unsigned draw(int quality, COLORREF fg, const char *s) {
    const int W = 90, H = 20;
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
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, quality,
        DEFAULT_PITCH | FF_DONTCARE, "DejaVu Sans");
    SelectObject(dc, f);
    SetTextColor(dc, fg); SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, 2, 2, s, (int)strlen(s));
    GdiFlush();
    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    DeleteObject(f); DeleteObject(hbm); DeleteDC(dc);
    return h;
}

int main(void) {
    printf("default=%08x cleartype=%08x colored=%08x descenders=%08x\n",
        draw(DEFAULT_QUALITY, RGB(0, 0, 0), "Hello, ARET!"),
        draw(CLEARTYPE_QUALITY, RGB(0, 0, 0), "Hello, ARET!"),
        draw(DEFAULT_QUALITY, RGB(0xE0, 0x20, 0x40), "Way"),
        draw(DEFAULT_QUALITY, RGB(0, 0, 0), "gyjpq"));
    printf("done\n");
    return 0;
}
