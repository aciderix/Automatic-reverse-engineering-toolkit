/* Menu long-tail family (display-free): ModifyMenuA/W (replace an item in place, by
 * command or by position), SetMenuItemBitmaps (associate check-mark bitmaps), and
 * GetMenuCheckMarkDimensions (default 13x13, MAKELONG(SM_CXMENUCHECK,SM_CYMENUCHECK)).
 * All values measured against Wine and deterministic; no display (NAME.nodisplay). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    printf("cxcheck=%d cycheck=%d\n",
           GetSystemMetrics(SM_CXMENUCHECK), GetSystemMetrics(SM_CYMENUCHECK));
    DWORD d = GetMenuCheckMarkDimensions();
    printf("checkdim=%08lx cx=%d cy=%d\n", (unsigned long)d, LOWORD(d), HIWORD(d));

    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, 100, "Alpha");
    AppendMenuA(m, MF_STRING, 101, "Beta");

    /* Force each mutation before its read (printf arg order is unspecified). */
    BOOL r1 = ModifyMenuA(m, 100, MF_BYCOMMAND | MF_STRING | MF_GRAYED, 200, "Gamma");
    BOOL r2 = ModifyMenuA(m, 1, MF_BYPOSITION | MF_STRING, 201, "Delta");
    BOOL r3 = ModifyMenuA(m, 999, MF_BYCOMMAND | MF_STRING, 202, "None");
    printf("modify r1=%d r2=%d r3=%d\n", r1, r2, r3);

    char b0[32] = {0}, b1[32] = {0};
    GetMenuStringA(m, 0, b0, sizeof b0, MF_BYPOSITION);
    GetMenuStringA(m, 1, b1, sizeof b1, MF_BYPOSITION);
    UINT id0 = GetMenuItemID(m, 0), id1 = GetMenuItemID(m, 1);
    UINT st0 = GetMenuState(m, 200, MF_BYCOMMAND);
    printf("s0=[%s](id=%u) s1=[%s](id=%u) st200=%x cnt=%d\n",
           b0, id0, b1, id1, (unsigned)st0, GetMenuItemCount(m));

    /* ModifyMenuW: replace pos 0 with a wide-string label. */
    BOOL rw = ModifyMenuW(m, 0, MF_BYPOSITION | MF_STRING, 300, L"Wide");
    char bw[32] = {0};
    GetMenuStringA(m, 0, bw, sizeof bw, MF_BYPOSITION);
    printf("modifyW=%d s0w=[%s](id=%u)\n", rw, bw, (unsigned)GetMenuItemID(m, 0));

    BOOL sb1 = SetMenuItemBitmaps(m, 0, MF_BYPOSITION, NULL, NULL);
    BOOL sb2 = SetMenuItemBitmaps(m, 777, MF_BYCOMMAND, NULL, NULL);
    printf("bitmaps ok=%d miss=%d\n", sb1, sb2);

    DestroyMenu(m);
    printf("done\n");
    return 0;
}
