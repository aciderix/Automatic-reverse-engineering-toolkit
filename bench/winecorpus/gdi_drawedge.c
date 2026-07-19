/* DrawEdge into a 32bpp DIB — the 3D bevel every button / group-box / edit-border uses.
 * The exact RGB comes from GetSysColor, which differs by THEME (ARET renders the classic
 * Win95 scheme, Wine 9.0 a modern light one), so we do NOT compare raw pixels. Instead
 * we verify the STRUCTURE theme-independently: each edge pixel must equal the system
 * color of the INDEX the bevel is supposed to use there. Both ARET and Wine then print
 * the same 1s (each against its own palette), so the layout + index mapping is proven
 * bit-for-bit without depending on the palette. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    const int W = 44, H = 16;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, hbm);
    unsigned *px = (unsigned *)bits;
    for (int i = 0; i < W * H; i++) px[i] = 0x00C0C0C0;
#define AT(x, y) (px[(y) * W + (x)] & 0xFFFFFF)
#define SC(i)    (GetSysColor(i) & 0xFFFFFF)

    RECT r = { 2, 2, 20, 14 };
    DrawEdge(dc, &r, EDGE_RAISED, BF_RECT);
    printf("raised oTL=%d oBR=%d iTL=%d iBR=%d\n",
           AT(2, 2)   == SC(COLOR_3DLIGHT),      /* outer top-left  */
           AT(19, 13) == SC(COLOR_3DDKSHADOW),   /* outer bot-right */
           AT(3, 3)   == SC(COLOR_BTNHIGHLIGHT), /* inner top-left  */
           AT(18, 12) == SC(COLOR_BTNSHADOW));   /* inner bot-right */
    /* corner precedence: bottom/right dark line wins at TR and BL */
    printf("raised TRdark=%d BLdark=%d topLTlight=%d\n",
           AT(19, 2) == SC(COLOR_3DDKSHADOW), AT(2, 13) == SC(COLOR_3DDKSHADOW), AT(10, 2) == SC(COLOR_3DLIGHT));

    RECT r2 = { 24, 2, 42, 14 };
    DrawEdge(dc, &r2, EDGE_SUNKEN, BF_RECT);
    printf("sunken oTL=%d oBR=%d iTL=%d iBR=%d\n",
           AT(24, 2)  == SC(COLOR_BTNSHADOW),
           AT(41, 13) == SC(COLOR_BTNHIGHLIGHT),
           AT(25, 3)  == SC(COLOR_3DDKSHADOW),
           AT(40, 12) == SC(COLOR_3DLIGHT));

    /* BF_MIDDLE fills the interior with 3DFACE */
    RECT r3 = { 2, 2, 20, 14 };
    DrawEdge(dc, &r3, EDGE_RAISED, BF_RECT | BF_MIDDLE);
    printf("middle face=%d\n", AT(10, 8) == SC(COLOR_3DFACE));
    printf("done\n");
    return 0;
}
