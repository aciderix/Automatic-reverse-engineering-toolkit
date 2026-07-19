/* Tabbed GDI text (extends the G3-text FreeType family): TabbedTextOutA/W +
 * GetTabbedTextExtentA/W. Draws into a 32bpp memory DIB with FreeType (the same
 * rasterizer Wine uses) so glyph pixels are bit-identical; the tab-stop layout recipe
 * is measured bit-exact vs Wine:
 *   - segment width = default advances (GetTextExtentPoint32);
 *   - the pen jumps to the next tab stop strictly greater than the pen;
 *   - stops: >1 positions -> absolute org+lpTabPos[j]; <=1 or beyond -> multiples of
 *     defWidth = lpTabPos[0] (one stop) else 8*tmAveCharWidth;
 *   - return = MAKELONG(totalWidth, tmHeight).
 * Oracle = extent/return values + an FNV hash of the whole DIB, bit-compared to Wine.
 * NONANTIALIASED (mono), regular weight, TRANSPARENT background — the proven subset.
 * Needs a display for Wine's GDI init (Xvfb), like the other GDI fixtures. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    const int W = 200, H = 24;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits;
    for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;   /* white background */

    HFONT f = CreateFontA(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, "DejaVu Sans");
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, TRANSPARENT);

    INT t1[1] = { 64 };
    INT t2[2] = { 40, 100 };
    /* extents (no draw): uniform, array, beyond-last, default (n=0) */
    DWORD e_uni  = GetTabbedTextExtentA(dc, "A\tB", 3, 1, t1);
    DWORD e_arr  = GetTabbedTextExtentA(dc, "A\tB\tC\tD", 7, 2, t2);
    DWORD e_def  = GetTabbedTextExtentA(dc, "A\tB", 3, 0, NULL);
    printf("ext uni=%08lx arr=%08lx def=%08lx\n",
           (unsigned long)e_uni, (unsigned long)e_arr, (unsigned long)e_def);

    /* real draws: the return value carries the extent; the DIB carries the pixels */
    LONG r1 = TabbedTextOutA(dc, 2, 2, "A\tB\tC\tD", 7, 2, t2, 0);
    LONG r2 = TabbedTextOutA(dc, 2, 12, "Hi\tThere", 8, 1, t1, 0);
    printf("tto r1=%08lx r2=%08lx\n", (unsigned long)r1, (unsigned long)r2);
    GdiFlush();

    unsigned h = 2166136261u;
    for (int i = 0; i < W * H; i++) { unsigned c = px[i] & 0xFFFFFF;
        h ^= (c & 0xFF); h *= 16777619u; h ^= ((c >> 8) & 0xFF); h *= 16777619u; h ^= ((c >> 16) & 0xFF); h *= 16777619u; }
    /* bbox of drawn (non-white) pixels — a human-readable cross-check */
    int minx = W, miny = H, maxx = -1, maxy = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if ((px[y * W + x] & 0xFFFFFF) != 0xFFFFFF) {
                if (x < minx) minx = x; if (x > maxx) maxx = x;
                if (y < miny) miny = y; if (y > maxy) maxy = y;
            }
    printf("dibhash=%08x bbox=%d %d %d %d\n", h, minx, miny, maxx, maxy);
    DeleteDC(dc); DeleteObject(f); DeleteObject(hbm);
    printf("done\n");
    return 0;
}
