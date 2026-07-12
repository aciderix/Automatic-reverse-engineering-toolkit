/* GDI UI-font substitution + stock fonts (G3, doc 72), bit-identical to Wine.
 *  - The classic Windows UI sans faces (MS Sans Serif, MS Shell Dlg, Tahoma…)
 *    resolve to the same metric-compatible replacement Wine uses (Liberation Sans),
 *    so they render identically to each other and to an explicit "Liberation Sans".
 *  - Stock fonts DEFAULT_GUI_FONT and ANSI_VAR_FONT carry the LOGFONT Wine reports
 *    and render bit-exactly. (Rendered with DEFAULT_QUALITY → subpixel.)
 * Needs a display (Xvfb). */
#include <windows.h>
#include <stdio.h>

static unsigned draw_face(const char *face) {
    const int W = 80, H = 20;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;
    HFONT f = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
        0, 0, DEFAULT_QUALITY, 0, face);
    SelectObject(dc, f);
    SetTextColor(dc, RGB(0, 0, 0)); SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, 2, 2, "Way123", 6); GdiFlush();
    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    DeleteObject(f); DeleteObject(hbm); DeleteDC(dc);
    return h;
}
static unsigned draw_stock(int id) {
    const int W = 80, H = 20;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits; for (int i = 0; i < W * H; i++) px[i] = 0x00FFFFFF;
    SelectObject(dc, GetStockObject(id));
    SetTextColor(dc, RGB(0, 0, 0)); SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, 2, 2, "Way123", 6); GdiFlush();
    unsigned h = 2166136261u; unsigned char *p = (unsigned char *)bits;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    DeleteObject(hbm); DeleteDC(dc);
    return h;
}

int main(void) {
    printf("mssans=%08x msshell=%08x tahoma=%08x libsans=%08x\n",
        draw_face("MS Sans Serif"), draw_face("MS Shell Dlg"),
        draw_face("Tahoma"), draw_face("Liberation Sans"));
    printf("gui=%08x ansivar=%08x\n", draw_stock(DEFAULT_GUI_FONT), draw_stock(ANSI_VAR_FONT));
    printf("done\n");
    return 0;
}
