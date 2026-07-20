/* DrawEdge(EDGE_ETCHED, BF_RECT) — the group-box frame: an etched groove (outer
 * top-left BTNSHADOW / bottom-right BTNHIGHLIGHT; inner top-left BTNHIGHLIGHT /
 * bottom-right BTNSHADOW). Verified STRUCTURALLY (index-based, theme-independent:
 * pixel == GetSysColor(expected INDEX) prints the same 1 on ARET-classic and
 * Wine-modern) into a memory DIB. Bit-exact. */
#include <windows.h>
#include <stdio.h>
int main(void) {
    const int W = 24, H = 20;
    HDC md = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *b = NULL; HBITMAP hb = CreateDIBSection(md, &bi, DIB_RGB_COLORS, &b, NULL, 0); SelectObject(md, hb);
    unsigned *px = (unsigned *)b; for (int i = 0; i < W * H; i++) px[i] = 0x00FF00FF;
    RECT r = { 0, 0, W, H }; DrawEdge(md, &r, EDGE_ETCHED, BF_RECT);
#define AT(x, y) (px[(y) * W + (x)] & 0xFFFFFF)
#define SC(i)    (GetSysColor(i) & 0xFFFFFF)
    printf("oTL=%d iTL=%d oBR=%d iBR=%d\n",
           AT(0, 0) == SC(COLOR_BTNSHADOW), AT(1, 1) == SC(COLOR_BTNHIGHLIGHT),
           AT(W - 1, H - 1) == SC(COLOR_BTNHIGHLIGHT), AT(W - 2, H - 2) == SC(COLOR_BTNSHADOW));
    printf("done\n");
    return 0;
}
