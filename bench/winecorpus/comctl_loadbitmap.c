/* M7 G7 — LoadBitmap: decode a packed DIB from an RT_BITMAP resource into an
 * HBITMAP the GDI model can blit (how real apps load toolbar/UI images, which
 * then feed ImageList). Load the embedded 4x2 24bpp bitmap, select it into a
 * memory DC, and read the pixels back — bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
int main(void) {
    HBITMAP bm = LoadBitmapA(GetModuleHandleA(NULL), MAKEINTRESOURCE(100));
    printf("loaded=%d\n", bm != NULL);
    BITMAP bi; memset(&bi, 0, sizeof bi); GetObjectA(bm, sizeof bi, &bi);
    printf("size=%ldx%ld bpp=%d\n", (long)bi.bmWidth, (long)bi.bmHeight, bi.bmBitsPixel);
    HDC dc = CreateCompatibleDC(NULL); SelectObject(dc, bm);
    printf("px00=%06lX px10=%06lX px01=%06lX px31=%06lX\n",
           (unsigned long)GetPixel(dc, 0, 0), (unsigned long)GetPixel(dc, 1, 0),
           (unsigned long)GetPixel(dc, 0, 1), (unsigned long)GetPixel(dc, 3, 1));
    DeleteDC(dc); DeleteObject(bm);
    printf("done\n");
    return 0;
}
