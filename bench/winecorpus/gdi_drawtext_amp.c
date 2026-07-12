/* GDI DrawText '&' accelerator prefix (G3, doc 72), bit-identical to Wine. Unless
 * DT_NOPREFIX, a single '&' is removed and marks the next char as the underlined
 * accelerator (a 1px pen line at baseline+1 spanning the char extent minus one);
 * '&&' is a literal '&'; a trailing '&' is dropped. Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static unsigned draw(const char *txt, unsigned fmt) {
    const int W = 90, H = 24;
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
        0, 0, NONANTIALIASED_QUALITY, 0, "DejaVu Sans");
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0)); SetBkMode(dc, TRANSPARENT);
    RECT rc = { 2, 2, 88, 22 };
    DrawTextA(dc, txt, -1, &rc, fmt | DT_SINGLELINE);
    GdiFlush();
    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    DeleteObject(f); DeleteObject(hbm); DeleteDC(dc);
    return h;
}

int main(void) {
    printf("amp=%08x noprefix=%08x plain=%08x mid=%08x dblamp=%08x\n",
        draw("&File", 0), draw("&File", DT_NOPREFIX), draw("File", 0),
        draw("Save &As", 0), draw("A && B", 0));
    printf("done\n");
    return 0;
}
