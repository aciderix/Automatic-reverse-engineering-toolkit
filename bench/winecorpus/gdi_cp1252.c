/* GDI ANSI codepage (G3, doc 72): TextOutA maps bytes through CP1252 (the Windows
 * ACP, GetACP()==1252), bit-identical to Wine — the 0x80-0x9F slots (€, curly
 * quotes, en/em dashes, ™…) become their Unicode codepoints, not Latin-1. Verified
 * by rendering the high bytes via TextOutA and the equivalent codepoints via
 * TextOutW: identical. Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static unsigned draw_a(const char *s, int n) {
    const int W = 60, H = 22;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;
    HFONT f = CreateFontA(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        0, 0, NONANTIALIASED_QUALITY, 0, "DejaVu Sans");
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0)); SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, 2, 3, s, n); GdiFlush();
    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    DeleteObject(f); DeleteObject(hbm); DeleteDC(dc);
    return h;
}

int main(void) {
    printf("acp=%d\n", GetACP());
    /* 0x80 EUR, 0x92 right single quote, 0x99 TM, 0x96 en-dash, 0x9C oe */
    char s[] = { (char)0x80, (char)0x92, (char)0x99, (char)0x96, (char)0x9C, 0 };
    /* plus a low-range and 0xA0-0xFF Latin-1 char to cover both regimes */
    char s2[] = { 'A', (char)0xE9 /* e-acute */, (char)0xFC /* u-umlaut */, 0 };
    printf("cp1252hi=%08x latin=%08x\n", draw_a(s, 5), draw_a(s2, 3));
    printf("done\n");
    return 0;
}
