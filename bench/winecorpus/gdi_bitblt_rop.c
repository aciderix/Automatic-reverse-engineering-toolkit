/* GDI BitBlt raster-ops (G6, doc 72), bit-identical to Wine. The common binary
 * (source,destination) ROP3 codes are per-pixel boolean combinations of the 32bpp
 * pixels: SRCCOPY(D=S), SRCAND(S&D), SRCPAINT(S|D), SRCINVERT(S^D), NOTSRCCOPY(~S),
 * DSTINVERT(~D), BLACKNESS(0), WHITENESS(~0). DIB hash oracle. Needs a display. */
#include <windows.h>
#include <stdio.h>

static HBITMAP mkdib(HDC dc, int W, int H, void **bits, unsigned fill) {
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HBITMAP h = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, NULL, 0);
    unsigned *p = *bits; for (int i = 0; i < W * H; i++) p[i] = fill;
    return h;
}
static unsigned test(DWORD rop) {
    const int W = 8, H = 8;
    HDC ddc = CreateCompatibleDC(NULL), sdc = CreateCompatibleDC(NULL);
    void *db, *sb;
    HBITMAP d = mkdib(ddc, W, H, &db, 0x00AA55CC), s = mkdib(sdc, W, H, &sb, 0x00330F0F);
    SelectObject(ddc, d); SelectObject(sdc, s);
    BitBlt(ddc, 0, 0, W, H, sdc, 0, 0, rop);
    GdiFlush();
    unsigned h = 2166136261u; unsigned char *p = db;
    for (int i = 0; i < W * H * 4; i++) { h ^= p[i]; h *= 16777619u; }
    DeleteObject(d); DeleteObject(s); DeleteDC(ddc); DeleteDC(sdc);
    return h;
}

int main(void) {
    printf("copy=%08x and=%08x paint=%08x invert=%08x notsrc=%08x dstinv=%08x black=%08x white=%08x\n",
        test(SRCCOPY), test(SRCAND), test(SRCPAINT), test(SRCINVERT),
        test(NOTSRCCOPY), test(DSTINVERT), test(BLACKNESS), test(WHITENESS));
    printf("done\n");
    return 0;
}
