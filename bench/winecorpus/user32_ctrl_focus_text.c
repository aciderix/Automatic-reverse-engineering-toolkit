/* Deterministic, Wine-verifiable parts of the focus + edit-text plumbing:
 * SetWindowText/GetWindowText on a predefined EDIT control (no app WNDPROC -> the text
 * is stored/reported by the default handler, as DefWindowProc does), and SetFocus/
 * GetFocus round-trip. (Actual typed keyboard input has no headless Wine oracle and is
 * verified qualitatively.) */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WP; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "FW"; RegisterClassA(&wc);
    HWND par = CreateWindowExA(0, "FW", "p", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 200, 150, NULL, NULL, GetModuleHandleA(0), NULL);
    HWND e = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_LEFT, 10, 10, 100, 20, par, (HMENU)1, GetModuleHandleA(0), NULL);
    SetWindowTextA(e, "hi");
    char b[32]; int n = GetWindowTextA(e, b, sizeof b);
    int len = GetWindowTextLengthA(e);
    SetFocus(e);
    printf("text=[%s] n=%d len=%d focus_eq=%d\n", b, n, len, GetFocus() == e);
    printf("done\n");
    return 0;
}
