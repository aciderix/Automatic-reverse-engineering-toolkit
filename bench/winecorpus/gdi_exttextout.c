/* GDI ExtTextOut (G3, doc 72): options + explicit rect + per-char spacing, bit-
 * identical to Wine. Covers plain (== TextOut), ETO_OPAQUE (fill the explicit
 * rect with bkColor), and lpDx (override each glyph's pen advance). Needs a
 * display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static HDC mk(void **bits, int W, int H) {
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, NULL, 0);
    SelectObject(dc, hbm);
    HFONT f = CreateFontA(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, "DejaVu Sans");
    SelectObject(dc, f); SetTextColor(dc, RGB(0, 0, 0));
    return dc;
}
static unsigned fnv(void *bits, int n) {
    unsigned h = 2166136261u; unsigned char *p = bits;
    for (int i = 0; i < n * 4; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

int main(void) {
    const int W = 90, H = 24; void *b;
    HDC dc = mk(&b, W, H); { unsigned *px = b; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF; }
    SetBkMode(dc, TRANSPARENT);
    ExtTextOutA(dc, 2, 3, 0, NULL, "Hi!", 3, NULL); GdiFlush();
    unsigned plain = fnv(b, W * H);

    dc = mk(&b, W, H); { unsigned *px = b; for (int i = 0; i < W * H; i++) px[i] = 0x00AABBCC; }
    SetBkColor(dc, RGB(0, 255, 0)); SetBkMode(dc, OPAQUE);
    RECT rc = { 5, 4, 60, 20 };
    ExtTextOutA(dc, 8, 6, ETO_OPAQUE, &rc, "Hi!", 3, NULL); GdiFlush();
    unsigned opaque = fnv(b, W * H);

    dc = mk(&b, W, H); { unsigned *px = b; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF; }
    SetBkMode(dc, TRANSPARENT); int dx[3] = { 12, 12, 12 };
    ExtTextOutA(dc, 2, 3, 0, NULL, "Hi!", 3, dx); GdiFlush();
    unsigned spaced = fnv(b, W * H);

    printf("plain=%08x opaque=%08x spaced=%08x\n", plain, opaque, spaced);
    printf("done\n");
    return 0;
}
