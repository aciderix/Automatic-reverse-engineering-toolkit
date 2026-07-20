/* DrawFrameControl(DFC_BUTTON, DFCS_BUTTONRADIO[|DFCS_CHECKED]) — the 13x13 radio glyph:
 * a small bevelled circle with a white field, plus the centre dot when checked. Verified
 * STRUCTURALLY (index-based, theme-independent) into a memory DIB. The glyph is a fixed
 * measured bitmap, so bit-exact despite being curved. */
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
    /* distinctive ring pixels (unambiguous indices) + the dot when checked */
    int darkL = AT(1, 5) == SC(COLOR_BTNSHADOW);      /* left edge shadow */
    int dkin  = AT(2, 5) == SC(COLOR_3DDKSHADOW);     /* inner dark ring */
    int lite  = AT(11, 5) == SC(COLOR_BTNHIGHLIGHT);  /* right edge highlight */
    int lite2 = AT(10, 5) == SC(COLOR_3DLIGHT);       /* inner light ring */
    int dot   = AT(6, 6) == SC(COLOR_WINDOWTEXT);     /* centre dot when checked */
    printf("state=0x%x darkL=%d dkin=%d lite=%d lite2=%d dot=%d\n", state, darkL, dkin, lite, lite2, dot);
    DeleteDC(md); DeleteObject(hb);
    return 0;
}
int main(void) { test(DFCS_BUTTONRADIO); test(DFCS_BUTTONRADIO | DFCS_CHECKED); printf("done\n"); return 0; }
