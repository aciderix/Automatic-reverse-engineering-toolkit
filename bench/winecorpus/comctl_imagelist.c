/* M7 G7 — common controls (comctl32): the ImageList data foundation of toolbars/
 * list-view/tree-view. Build a 2-tile source DIB, add it to an image list, draw
 * each image into a destination DIB, and read the pixels back — bit-identical to
 * Wine. (Images are modelled as opaque 32bpp tiles: a flat BI_RGB source carries
 * no alpha and an ILC_COLOR32 list applies no colour-key mask, matching Wine.) */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

static HBITMAP mkdib(int w, int h, unsigned char **bits) {
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

int main(void) {
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_BAR_CLASSES };
    printf("init=%d\n", InitCommonControlsEx(&ic));

    HIMAGELIST il = ImageList_Create(8, 8, ILC_COLOR32, 2, 0);
    printf("create=%d\n", il != NULL);
    int cx = 0, cy = 0; ImageList_GetIconSize(il, &cx, &cy);
    printf("iconsize=%dx%d\n", cx, cy);

    /* source: two 8x8 tiles (red, green) side by side */
    unsigned char *sb; HBITMAP src = mkdib(16, 8, &sb);
    HDC md = CreateCompatibleDC(NULL); SelectObject(md, src);
    RECT r0 = {0, 0, 8, 8}, r1 = {8, 0, 16, 8};
    HBRUSH b = CreateSolidBrush(RGB(200, 10, 10)); FillRect(md, &r0, b); DeleteObject(b);
    b = CreateSolidBrush(RGB(10, 200, 10)); FillRect(md, &r1, b); DeleteObject(b);
    int idx = ImageList_Add(il, src, NULL);   /* sequence: add before the count read */
    printf("add=%d count=%d\n", idx, ImageList_GetImageCount(il));

    /* dest 24x8, blue background; draw the two tiles */
    unsigned char *dbits; HBITMAP dst = mkdib(24, 8, &dbits);
    HDC dd = CreateCompatibleDC(NULL); SelectObject(dd, dst);
    RECT rf = {0, 0, 24, 8}; b = CreateSolidBrush(RGB(0, 0, 80)); FillRect(dd, &rf, b); DeleteObject(b);
    ImageList_Draw(il, 0, dd, 0, 0, ILD_NORMAL);
    ImageList_Draw(il, 1, dd, 8, 0, ILD_NORMAL);
    printf("px t0=%06lX t1=%06lX bg=%06lX\n",
           (unsigned long)GetPixel(dd, 2, 2),
           (unsigned long)GetPixel(dd, 10, 2),
           (unsigned long)GetPixel(dd, 20, 2));

    printf("setbk=%06lX\n", (unsigned long)ImageList_SetBkColor(il, RGB(1, 2, 3)));
    ImageList_Destroy(il);
    printf("done\n");
    return 0;
}
