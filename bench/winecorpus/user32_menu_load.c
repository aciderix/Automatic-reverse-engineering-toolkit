/* LoadMenuW parses the app's own RT_MENU (classic template) into the menu model, so an app
 * (notepad et al.) can load its menu bar from resources. We load a classic menu (companion
 * user32_menu_load.rc) and walk it — count, per-item text/state, and the submenu tree —
 * comparing byte-for-byte vs Wine. NB: proves menu CREATION/introspection, not rendering. */
#include <windows.h>
#include <stdio.h>
static void dump(HMENU m, int depth) {
    int n = GetMenuItemCount(m);
    for (int i = 0; i < n; i++) {
        wchar_t buf[64]; buf[0]=0;
        int len = GetMenuStringW(m, i, buf, 64, MF_BYPOSITION);
        char nb[128]; int k = 0; for (; buf[k] && k < 63; k++) nb[k] = (char)buf[k]; nb[k] = 0;
        UINT st = GetMenuState(m, i, MF_BYPOSITION);
        HMENU sub = GetSubMenu(m, i);
        for (int d = 0; d < depth; d++) printf("  ");
        printf("item %d: \"%s\" len=%d state=%#x hassub=%d\n", i, nb, len, (unsigned)st, sub ? 1 : 0);
        if (sub) dump(sub, depth + 1);
    }
}
int main(void) {
    HMENU m = LoadMenuW(GetModuleHandleW(0), MAKEINTRESOURCEW(1));
    if (!m) { printf("no menu\n"); return 1; }
    printf("top count=%d\n", GetMenuItemCount(m));
    dump(m, 0);
    printf("done\n");
    return 0;
}
