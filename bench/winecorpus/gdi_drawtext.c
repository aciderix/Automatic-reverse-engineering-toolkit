/* GDI DrawText (G3, doc 72): single-line layout within a rect, bit-identical to
 * Wine — horizontal DT_LEFT/CENTER/RIGHT, vertical DT_TOP/VCENTER/BOTTOM, and
 * DT_CALCRECT (measure only). The return value (text height, or offset to text
 * bottom for VCENTER/BOTTOM) and the pixels both match. Needs a display (Xvfb). */
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
    SelectObject(dc, f); SetTextColor(dc, RGB(0, 0, 0)); SetBkMode(dc, TRANSPARENT);
    return dc;
}
static unsigned fnv(void *b, int n) {
    unsigned h = 2166136261u; unsigned char *p = b;
    for (int i = 0; i < n * 4; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static void run(unsigned fmt, const char *tag) {
    const int W = 80, H = 30; void *b; HDC dc = mk(&b, W, H);
    { unsigned *px = b; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF; }
    RECT rc = { 4, 3, 74, 27 };
    int r = DrawTextA(dc, "Way", -1, &rc, fmt);
    GdiFlush();
    printf("%-18s ret=%d rc=(%ld,%ld,%ld,%ld) hash=%08x\n",
        tag, r, rc.left, rc.top, rc.right, rc.bottom, fnv(b, W * H));
    DeleteDC(dc);
}

int main(void) {
    run(DT_SINGLELINE | DT_LEFT | DT_TOP, "left|top");
    run(DT_SINGLELINE | DT_CENTER | DT_VCENTER, "center|vcenter");
    run(DT_SINGLELINE | DT_RIGHT | DT_BOTTOM, "right|bottom");
    run(DT_SINGLELINE | DT_CALCRECT, "calcrect");
    printf("done\n");
    return 0;
}
