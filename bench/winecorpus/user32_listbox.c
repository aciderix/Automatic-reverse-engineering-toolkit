/* LISTBOX (LB_*) and COMBOBOX (CB_*) item model — the deterministic, Wine-verifiable
 * part: add / count / set+get selection / get text / delete. (Item painting is theme-
 * and font-dependent -> verified qualitatively via screenshot, not here.) */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "LW"; RegisterClassA(&wc);
    HWND par = CreateWindowExA(0, "LW", "p", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 240, 220, NULL, NULL, GetModuleHandleA(0), NULL);
    HWND lb = CreateWindowExA(0, "LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 10, 10, 120, 90, par, (HMENU)1, GetModuleHandleA(0), NULL);
    SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"Apple");
    SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"Banana");
    SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)"Cherry");
    SendMessageA(lb, LB_INSERTSTRING, 1, (LPARAM)"Apricot");
    SendMessageA(lb, LB_SETCURSEL, 2, 0);
    char b[64]; SendMessageA(lb, LB_GETTEXT, 2, (LPARAM)b);
    printf("LB count=%d cursel=%d text2=[%s] len1=%d\n",
           (int)SendMessageA(lb, LB_GETCOUNT, 0, 0), (int)SendMessageA(lb, LB_GETCURSEL, 0, 0), b,
           (int)SendMessageA(lb, LB_GETTEXTLEN, 1, 0));
    SendMessageA(lb, LB_DELETESTRING, 0, 0);
    char b2[64]; SendMessageA(lb, LB_GETTEXT, 0, (LPARAM)b2);
    printf("LB afterdel count=%d text0=[%s]\n", (int)SendMessageA(lb, LB_GETCOUNT, 0, 0), b2);

    HWND cb = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 10, 110, 120, 90, par, (HMENU)2, GetModuleHandleA(0), NULL);
    SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"Red");
    SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"Green");
    SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"Blue");
    SendMessageA(cb, CB_SETCURSEL, 1, 0);
    char cbuf[64]; SendMessageA(cb, CB_GETLBTEXT, 1, (LPARAM)cbuf);
    printf("CB count=%d cursel=%d text1=[%s]\n",
           (int)SendMessageA(cb, CB_GETCOUNT, 0, 0), (int)SendMessageA(cb, CB_GETCURSEL, 0, 0), cbuf);
    DestroyWindow(par);
    printf("done\n");
    return 0;
}
