/* GDI DC state: SetMapMode (returns prev MM_TEXT), SaveDC/RestoreDC (restores the
 * text color), GetClipBox (surface bounds + SIMPLEREGION). Measured vs Wine. */
#include <windows.h>
#include <stdio.h>
int main(void) {
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = -4;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits; HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0); SelectObject(dc, hb);
    int mm0 = SetMapMode(dc, MM_TEXT);
    SetTextColor(dc, RGB(1, 2, 3));
    int lvl = SaveDC(dc);
    SetTextColor(dc, RGB(9, 9, 9));
    int rest = RestoreDC(dc, lvl);
    printf("mm0=%d save=%d restore=%d color=%06x\n", mm0, lvl, rest, (unsigned)GetTextColor(dc));
    RECT cb; int cx = GetClipBox(dc, &cb);
    printf("clip cx=%d r=%ld,%ld,%ld,%ld\n", cx, cb.left, cb.top, cb.right, cb.bottom);
    DeleteObject(hb); DeleteDC(dc);
    printf("done\n");
    return 0;
}
