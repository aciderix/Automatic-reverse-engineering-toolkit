/* Batch of window/dialog/DC helpers found by the M7 G7 corpus sweep (shared by
 * real Win95 GUI apps): RegisterWindowMessage (atom-like unique id), MapWindow-
 * Points (client<->screen), predefined control classes as data-only windows
 * (so CheckDlgButton/IsDlgButtonChecked work on a CreateWindowEx BUTTON child),
 * IsWindowUnicode, GetWindowWord/SetWindowWord, SetTextAlign/GetTextAlign,
 * BeginDeferWindowPos/DeferWindowPos/EndDeferWindowPos. All display-free,
 * bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.lpfnWndProc = WP; wc.lpszClassName = "WB"; RegisterClassA(&wc);
    UINT a = RegisterWindowMessageA("ARET_MsgFoo"), b = RegisterWindowMessageA("ARET_MsgFoo"), c = RegisterWindowMessageA("ARET_MsgBar");
    printf("rwm same=%d diff=%d range=%d\n", a == b, a != c, (a >= 0xC000 && a <= 0xFFFF));
    HWND par = CreateWindowExA(0, "WB", "p", WS_POPUP, 30, 40, 200, 150, NULL, NULL, NULL, NULL);
    POINT pt = {5, 7}; MapWindowPoints(par, NULL, &pt, 1);
    printf("map c2s=%ld,%ld\n", (long)pt.x, (long)pt.y);
    POINT pt2 = {35, 47}; MapWindowPoints(NULL, par, &pt2, 1);
    printf("map s2c=%ld,%ld\n", (long)pt2.x, (long)pt2.y);
    HWND cb = CreateWindowExA(0, "BUTTON", "c", WS_CHILD | BS_CHECKBOX, 0, 0, 10, 10, par, (HMENU)101, NULL, NULL);
    printf("cb_exists=%d cb0=%d\n", cb != NULL, IsDlgButtonChecked(par, 101));
    CheckDlgButton(par, 101, BST_CHECKED);   printf("cb1=%d\n", IsDlgButtonChecked(par, 101));
    CheckDlgButton(par, 101, BST_UNCHECKED); printf("cb2=%d\n", IsDlgButtonChecked(par, 101));
    printf("uni_a=%d\n", IsWindowUnicode(par));
    HDC dc = GetDC(par); UINT pa = SetTextAlign(dc, 6); UINT ga = GetTextAlign(dc); ReleaseDC(par, dc);
    printf("ta old=%u new=%u\n", pa, ga);
    HDWP hd = BeginDeferWindowPos(1);
    hd = DeferWindowPos(hd, par, NULL, 100, 110, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    EndDeferWindowPos(hd);
    RECT r; GetWindowRect(par, &r); printf("moved=%ld,%ld\n", (long)r.left, (long)r.top);
    DestroyWindow(par);
    printf("done\n");
    return 0;
}
