/* A top-level window gets its menu from ONE of two places, both handled by user32 at
 * CreateWindow time — not by an explicit LoadMenu/SetMenu:
 *   (a) the hMenu argument of CreateWindow (for a non-WS_CHILD window, arg 9 IS the menu);
 *   (b) the window class's lpszMenuName — if hMenu is NULL, user32 loads that class menu
 *       from the module's resources and attaches it.
 * notepad relies on (b) (its WNDCLASS names a MENU resource), which is why its menu never
 * appears when the attachment is missing. GetMenu(hwnd) must then return the right menu and
 * GetMenuItemCount/GetMenuString must see its items. Headless: we only query the model,
 * never paint. Proven against Wine via winediff. */
#include <windows.h>
#include <stdio.h>

#define ID_MENURES 100

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static void report(const char *tag, HWND h) {
    HMENU mn = GetMenu(h);
    int cnt = mn ? GetMenuItemCount(mn) : -1;
    char it0[64] = "";
    if (mn && cnt > 0) {
        wchar_t wb[64] = {0}; GetMenuStringW(mn, 0, wb, 63, MF_BYPOSITION);
        int i = 0; for (; wb[i] && i < 63; i++) it0[i] = (char)wb[i]; it0[i] = 0;
    }
    printf("%-14s GetMenu=%d count=%d item0=\"%s\"\n", tag, mn != NULL, cnt, it0);
}

int main(void) {
    HINSTANCE hi = GetModuleHandleW(NULL);

    /* (b) class carries lpszMenuName -> CreateWindow with hMenu=NULL auto-loads it */
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wp; wc.hInstance = hi; wc.lpszClassName = L"AretClassMenu";
    wc.lpszMenuName = MAKEINTRESOURCEW(ID_MENURES);
    RegisterClassW(&wc);
    HWND hb = CreateWindowExW(0, L"AretClassMenu", L"cm", WS_OVERLAPPEDWINDOW,
                              0, 0, 300, 200, NULL, NULL, hi, NULL);
    report("classmenu", hb);

    /* (a) explicit hMenu to CreateWindow overrides the class menu */
    WNDCLASSW wc2 = {0};
    wc2.lpfnWndProc = wp; wc2.hInstance = hi; wc2.lpszClassName = L"AretNoMenu";
    RegisterClassW(&wc2);
    HMENU expl = LoadMenuW(hi, MAKEINTRESOURCEW(ID_MENURES));
    HWND ha = CreateWindowExW(0, L"AretNoMenu", L"am", WS_OVERLAPPEDWINDOW,
                              0, 0, 300, 200, NULL, expl, hi, NULL);
    report("explicit", ha);

    /* no class menu, no hMenu -> GetMenu is NULL */
    HWND hn = CreateWindowExW(0, L"AretNoMenu", L"nm", WS_OVERLAPPEDWINDOW,
                              0, 0, 300, 200, NULL, NULL, hi, NULL);
    report("nomenu", hn);

    DestroyWindow(hb); DestroyWindow(ha); DestroyWindow(hn);
    printf("done\n");
    return 0;
}
