/* BUTTON check-state messages BM_SETCHECK / BM_GETCHECK on an auto-checkbox: the
 * deterministic, Wine-verifiable part of the button-control plumbing (the click
 * BM_CLICK/mouse path has no headless Wine oracle and is verified qualitatively).
 * Round-trip: set checked -> get 1, set unchecked -> get 0. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "BW"; RegisterClassA(&wc);
    HWND par = CreateWindowExA(0, "BW", "p", WS_OVERLAPPEDWINDOW, 0, 0, 200, 150, NULL, NULL, GetModuleHandleA(0), NULL);
    HWND c = CreateWindowExA(0, "BUTTON", "X", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 10, 10, 100, 20, par, (HMENU)1, GetModuleHandleA(0), NULL);
    printf("init=%d ", (int)SendMessageA(c, BM_GETCHECK, 0, 0));
    SendMessageA(c, BM_SETCHECK, BST_CHECKED, 0);   printf("set1=%d ", (int)SendMessageA(c, BM_GETCHECK, 0, 0));
    SendMessageA(c, BM_SETCHECK, BST_UNCHECKED, 0); printf("set0=%d\n", (int)SendMessageA(c, BM_GETCHECK, 0, 0));
    DestroyWindow(par);
    printf("done\n");
    return 0;
}
