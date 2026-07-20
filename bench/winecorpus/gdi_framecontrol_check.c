/* DrawFrameControl(DFC_BUTTON, DFCS_BUTTONCHECK[|DFCS_CHECKED]) — the 13x13 check-box
 * glyph: a sunken edge + white field, plus the Marlett tick when checked. Verified
 * STRUCTURALLY (index-based, theme-independent: pixel == GetSysColor(expected INDEX)
 * prints the same 1 on ARET-classic and Wine-modern) into a memory DIB. Bit-exact. */
#include <windows.h>
#include <stdio.h>
static int test(UINT state) {
    const int W = 13, H = 13;
    HDC md = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *b = NULL; HBITMAP hb = CreateDIBSection(md, &bi, DIB_RGB_COLORS, &b, NULL, 0); SelectObject(md, hb);
    unsigned *px = (unsigned *)b; for (int i = 0; i < W * H; i++) px[i] = 0x00FF00FF;
    RECT r = { 0, 0, W, H }; DrawFrameControl(md, &r, DFC_BUTTON, state);
#define AT(x, y) (px[(y) * W + (x)] & 0xFFFFFF)
#define SC(i)    (GetSysColor(i) & 0xFFFFFF)
    /* box structure (sunken): outer TL=BTNSHADOW, inner TL=3DDKSHADOW, inner BR=3DLIGHT,
     * outer BR=BTNHIGHLIGHT; interior=COLOR_WINDOW. tick pixel (in WINDOWTEXT) when checked. */
    int oTL = AT(0, 0) == SC(COLOR_BTNSHADOW);
    int iTL = AT(1, 1) == SC(COLOR_3DDKSHADOW);
    int iBR = AT(11, 11) == SC(COLOR_3DLIGHT);
    int oBR = AT(12, 12) == SC(COLOR_BTNHIGHLIGHT);
    int field = AT(6, 2) == SC(COLOR_WINDOW);
    int tick = AT(5, 7) == SC(COLOR_WINDOWTEXT);   /* a mark pixel when checked */
    printf("state=0x%x oTL=%d iTL=%d iBR=%d oBR=%d field=%d tick=%d\n", state, oTL, iTL, iBR, oBR, field, tick);
    DeleteDC(md); DeleteObject(hb);
    return 0;
}
int main(void) {
    test(DFCS_BUTTONCHECK);
    test(DFCS_BUTTONCHECK | DFCS_CHECKED);
    printf("done\n");
    return 0;
}
