/* IsDialogMessage — dialog keyboard navigation. Feeds synthetic WM_KEYDOWN messages
   (Tab / Enter / Escape) and reports the return value and resulting focus, measured vs
   Wine so our implementation matches: Tab moves to the next tab-stop control, Enter and
   Escape are consumed (they map to the default/cancel command). A parent window with
   three tab-stop children; runs under the harness Xvfb. */
#include <windows.h>
#include <stdio.h>

#define ID1 101
#define ID2 102
#define ID3 103
static HWND c1, c2, c3;
static const char *nm(HWND h) { return h == c1 ? "c1" : h == c2 ? "c2" : h == c3 ? "c3" : h == NULL ? "0" : "?"; }

static HWND mk(HWND p, const char *cls, DWORD st, int id) {
    return CreateWindowExA(0, cls, "x", WS_CHILD | WS_VISIBLE | WS_TABSTOP | st, 0, 0, 60, 16,
                           p, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
}
int main(void) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = hi; wc.lpszClassName = "idm";
    RegisterClassA(&wc);
    HWND p = CreateWindowExA(WS_EX_CONTROLPARENT, "idm", "", WS_OVERLAPPEDWINDOW,
                             0, 0, 200, 120, NULL, NULL, hi, NULL);
    c1 = mk(p, "BUTTON", BS_PUSHBUTTON, ID1);
    c2 = mk(p, "BUTTON", BS_PUSHBUTTON, ID2);
    c3 = mk(p, "EDIT", 0, ID3);
    ShowWindow(p, SW_SHOW);

    MSG msg; memset(&msg, 0, sizeof msg);
    SetFocus(c1);
    msg.hwnd = GetFocus(); msg.message = WM_KEYDOWN; msg.wParam = VK_TAB;
    BOOL r = IsDialogMessageA(p, &msg);
    printf("tab:   ret=%d focus=%s\n", r, nm(GetFocus()));

    SetFocus(c1);
    msg.hwnd = GetFocus(); msg.wParam = VK_TAB;
    r = IsDialogMessageA(p, &msg);       /* again: c2 -> c3 */
    SetFocus(c3);
    msg.hwnd = GetFocus(); msg.wParam = VK_TAB;
    r = IsDialogMessageA(p, &msg);       /* wrap c3 -> c1 */
    printf("tabwrap: ret=%d focus=%s\n", r, nm(GetFocus()));

    SetFocus(c1);
    msg.hwnd = GetFocus(); msg.wParam = VK_RETURN;
    r = IsDialogMessageA(p, &msg);
    printf("enter: ret=%d\n", r);

    msg.hwnd = GetFocus(); msg.wParam = VK_ESCAPE;
    r = IsDialogMessageA(p, &msg);
    printf("esc:   ret=%d\n", r);
    return 0;
}
