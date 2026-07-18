/* GDI mapping mode (logical->device coordinate transform): SetMapMode(
   MM_ANISOTROPIC) + Set{Window,Viewport}{Ext,Org}Ex set an affine transform that
   LPtoDP/DPtoLP apply and that every drawing primitive honours. Verified
   bit-identical to Wine: the coordinate round-trip AND the pixels of lines/
   rectangles drawn under the transform (DIB hash). */
#include <windows.h>
#include <stdio.h>
static unsigned fnv(const unsigned char *p, int n) {
    unsigned h = 2166136261u; for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h;
}
int main(void) {
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = 40; bi.bmiHeader.biWidth = 40; bi.bmiHeader.biHeight = -30;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *pv; HBITMAP bmp = CreateDIBSection(dc, &bi, 0, &pv, NULL, 0); SelectObject(dc, bmp);
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, 10, 10, NULL); SetViewportExtEx(dc, 20, 10, NULL);
    SetWindowOrgEx(dc, 1, 1, NULL);   SetViewportOrgEx(dc, 3, 2, NULL);
    POINT p = {7, 5}; LPtoDP(dc, &p, 1); POINT q = p; DPtoLP(dc, &q, 1);
    printf("lptodp=(%ld,%ld) back=(%ld,%ld)\n", p.x, p.y, q.x, q.y);
    SIZE vp; GetViewportExtEx(dc, &vp); ScaleViewportExtEx(dc, 3, 2, 1, 1, NULL);
    SIZE vp2; GetViewportExtEx(dc, &vp2);
    printf("vpext=(%ld,%ld)->scaled(%ld,%ld)\n", vp.cx, vp.cy, vp2.cx, vp2.cy);
    /* reset scale, draw under the transform */
    SetViewportExtEx(dc, 20, 10, NULL);
    MoveToEx(dc, 1, 1, NULL); LineTo(dc, 15, 8);
    Rectangle(dc, 2, 2, 7, 6);
    SetPixel(dc, 4, 4, RGB(255, 0, 0));
    printf("dibhash=%08x\n", fnv((unsigned char *)pv, 40 * 30 * 4));
    return 0;
}
