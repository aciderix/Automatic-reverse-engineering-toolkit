/* Exercises the USER32 menu data model (long-tail, display-free): CreatePopupMenu,
 * AppendMenuA (string/separator/checked), GetMenuItemCount, GetMenuState, Enable/
 * CheckMenuItem (return previous state), GetMenuStringA. All values measured against
 * Wine and deterministic; menu objects need no display (NAME.nodisplay). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    HMENU m = CreatePopupMenu();
    printf("create=%d\n", m != NULL);
    AppendMenuA(m, MF_STRING, 100, "File");
    AppendMenuA(m, MF_STRING, 101, "Edit");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING | MF_CHECKED, 102, "Word Wrap");
    printf("count=%d\n", GetMenuItemCount(m));
    printf("state100=%08x state102=%08x\n",
           (unsigned)GetMenuState(m, 100, MF_BYCOMMAND), (unsigned)GetMenuState(m, 102, MF_BYCOMMAND));
    /* Separate statements: force the mutation before the read (printf arg order is
     * unspecified), so this actually verifies EnableMenuItem/CheckMenuItem change state. */
    UINT ep = EnableMenuItem(m, 101, MF_BYCOMMAND | MF_GRAYED);
    UINT s101 = GetMenuState(m, 101, MF_BYCOMMAND);
    printf("enable_prev=%d state101=%08x\n", (int)ep, (unsigned)s101);
    UINT cp = CheckMenuItem(m, 100, MF_BYCOMMAND | MF_CHECKED);
    UINT s100 = GetMenuState(m, 100, MF_BYCOMMAND);
    printf("check_prev=%d state100=%08x\n", (int)cp, (unsigned)s100);
    char buf[32]; int n = GetMenuStringA(m, 100, buf, sizeof buf, MF_BYCOMMAND);
    printf("str n=%d [%s]\n", n, buf);
    printf("pos0state=%08x\n", (unsigned)GetMenuState(m, 0, MF_BYPOSITION));
    DestroyMenu(m);
    printf("done\n");
    return 0;
}
