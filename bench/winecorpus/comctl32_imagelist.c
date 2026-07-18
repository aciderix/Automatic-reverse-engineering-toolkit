/* DLL lifting on a REAL system DLL (doc 80 §1.2): comctl32.dll is lifted from
   Wine's own PE builtin (winecorpus/comctl32_imagelist.withdll) and its ImageList
   API dispatches to that lifted code, running on ARET's HLE gdi32 — bit-identical
   to Wine loading the real comctl32. The first real comctl32 control feature. */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
int main(void) {
    HIMAGELIST il = ImageList_Create(16, 16, ILC_COLOR32, 4, 4);
    printf("create=%d\n", il != NULL);
    printf("count0=%d\n", ImageList_GetImageCount(il));
    HBITMAP bmp = CreateBitmap(16, 16, 1, 32, NULL);
    int idx = ImageList_Add(il, bmp, NULL);
    printf("add_idx=%d count1=%d\n", idx, ImageList_GetImageCount(il));
    int cx = 0, cy = 0; ImageList_GetIconSize(il, &cx, &cy);
    printf("iconsize=%dx%d\n", cx, cy);
    printf("remove=%d count2=%d\n", ImageList_Remove(il, 0), ImageList_GetImageCount(il));
    ImageList_Destroy(il);
    return 0;
}
