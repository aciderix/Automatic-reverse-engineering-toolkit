/* GDI text raster (G3-text, doc 72): TextOut into a 32bpp memory DIB, rasterized
 * with FreeType — the SAME rasterizer Wine uses — so the glyph bitmap, baseline
 * (Wine's usWinAscent metric), positioning and advances are bit-identical to Wine.
 * The binary stays autonomous (FreeType statically linkable, no Wine runtime).
 *
 * Subset proven here (the rest aborts soundly in ARET until later increments):
 * NONANTIALIASED_QUALITY (mono), regular upright weight, TRANSPARENT background,
 * default TA_TOP|TA_LEFT alignment, 32bpp DIB target. Face "DejaVu Sans" resolves
 * to the same file (DejaVuSans.ttf) in both Wine and fontconfig.
 *
 * Oracle = a compact ASCII map of the drawn pixels + an FNV hash of the whole
 * buffer, printed to stdout → winediff compares bit-for-bit. Needs a display for
 * Wine's GDI init (Xvfb), like the other GDI fixtures. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    const int W = 96, H = 40;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    /* white background so the glyph pixels stand out */
    unsigned *px = (unsigned *)bits;
    for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;

    HFONT f = CreateFontA(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, "DejaVu Sans");
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    BOOL ok = TextOutA(dc, 2, 3, "Hello, ARET!", 12);
    /* a coloured second line: exercises the COLORREF↔[B,G,R,0] byte order (a
     * red/blue swap would be invisible with black text alone). */
    SetTextColor(dc, RGB(0xE0, 0x20, 0x40));
    ok = TextOutA(dc, 2, 21, "rgb", 3) && ok;
    GdiFlush();
    printf("textout ok=%d\n", ok != 0);

    /* bbox of the non-white pixels + ASCII map (exact, human-readable) */
    int minx = W, miny = H, maxx = -1, maxy = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned c = px[y * W + x] & 0xFFFFFF;
            if (c != 0xFFFFFF) { if (x<minx)minx=x; if (x>maxx)maxx=x; if (y<miny)miny=y; if (y>maxy)maxy=y; }
        }
    printf("bbox %d %d %d %d\n", minx, miny, maxx, maxy);
    for (int y = miny; y <= maxy && y >= 0; y++) {
        for (int x = minx; x <= maxx; x++)
            putchar((px[y * W + x] & 0xFFFFFF) == 0xFFFFFF ? '.' : '#');
        putchar('\n');
    }

    unsigned char *p = (unsigned char *)bits; unsigned h = 2166136261u;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    printf("hash=%08x\n", h);

    DeleteObject(f); DeleteObject(hbm); DeleteDC(dc);
    printf("done\n");
    return 0;
}
