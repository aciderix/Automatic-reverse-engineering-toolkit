/* GDI multi-line DrawText (G3, doc 72): DT_WORDBREAK word-wrap + explicit '\n',
 * bit-identical to Wine. Lines are split on '\n' and greedily wrapped at the last
 * space that keeps the line within the rect width; each line is drawn at
 * rc.top + i*tmHeight with per-line alignment. The return value (height of the
 * lines that fit, or all lines for DT_CALCRECT), the CALCRECT rect (widest line ×
 * total height, trailing spaces excluded), and the pixels all match Wine. Needs a
 * display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static HDC mk(void **b, int W, int H) {
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, b, NULL, 0);
    SelectObject(dc, hbm);
    HFONT f = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
        0, 0, NONANTIALIASED_QUALITY, 0, "DejaVu Sans");
    SelectObject(dc, f); SetTextColor(dc, RGB(0, 0, 0)); SetBkMode(dc, TRANSPARENT);
    return dc;
}
static unsigned fnv(void *b, int n) {
    unsigned h = 2166136261u; unsigned char *p = b;
    for (int i = 0; i < n * 4; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static void run(const char *txt, unsigned fmt, const char *tag) {
    const int W = 80, H = 60; void *b; HDC dc = mk(&b, W, H);
    { unsigned *px = b; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF; }
    RECT rc = { 2, 2, 60, 55 };
    int r = DrawTextA(dc, txt, -1, &rc, fmt);
    GdiFlush();
    printf("%-16s ret=%d rc=(%ld,%ld,%ld,%ld) hash=%08x\n",
        tag, r, rc.left, rc.top, rc.right, rc.bottom, fnv(b, W * H));
    DeleteDC(dc);
}

int main(void) {
    const char *t = "The quick brown fox jumps";
    run(t, DT_WORDBREAK, "wordbreak");
    run(t, DT_WORDBREAK | DT_CALCRECT, "wb_calc");
    run("Line1\nLine2\nL3", DT_LEFT, "explicit_nl");
    run(t, DT_WORDBREAK | DT_CENTER, "wb_center");
    run(t, DT_WORDBREAK | DT_RIGHT, "wb_right");
    printf("done\n");
    return 0;
}
