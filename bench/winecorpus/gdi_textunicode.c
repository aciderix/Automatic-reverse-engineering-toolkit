/* GDI wide text (G3, doc 72): TextOutW / GetTextExtentPoint32W with real Unicode
 * (not narrowed to bytes) — each UTF-16 unit maps through the font cmap, bit-
 * identical to Wine. Covers accented Latin and a non-Latin script (Greek). Needs
 * a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static unsigned draw(const wchar_t *ws, int n) {
    const int W = 80, H = 22;
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
    TextOutW(dc, 2, 3, ws, n);
    GdiFlush();
    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    DeleteObject(f); DeleteObject(hbm); DeleteDC(dc);
    return h;
}

int main(void) {
    wchar_t accent[] = { 0x00E9, 0x00E8, 0x00FC, 0x00F1, 0 };  /* é è ü ñ */
    wchar_t greek[]  = { 0x03B1, 0x03B2, 0x03B3, 0 };          /* α β γ  */
    HDC dc = CreateCompatibleDC(NULL);
    HFONT f = CreateFontA(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, "DejaVu Sans");
    SelectObject(dc, f);
    SIZE sz; GetTextExtentPoint32W(dc, accent, 4, &sz);
    printf("accent=%08x greek=%08x extentW=%ld\n", draw(accent, 4), draw(greek, 3), sz.cx);
    printf("done\n");
    return 0;
}
