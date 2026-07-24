/* Subclassing a predefined control (SetWindowLong GWL_WNDPROC) and chaining the
   original proc via CallWindowProc — the MFC pattern (AfxWndProc chains to the saved
   control proc for messages it doesn't handle). The subclass forwards BM_SETCHECK to
   the control's original proc; that must still apply the check (and BM_GETCHECK read
   it back), i.e. CallWindowProc must run the system control behaviour. Display-free-ish
   message round-trip; runs under the harness Xvfb (a real control window). */
#include <windows.h>
#include <stdio.h>

#define ID_CHK 1001
static WNDPROC g_orig;
static int g_saw = 0;

static LRESULT CALLBACK Sub(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == BM_SETCHECK) g_saw = 1;           /* the subclass sees it, then chains */
    return CallWindowProcA(g_orig, h, m, w, l);
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcA(h, m, w, l);
}

int main(void) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = "subtest";
    RegisterClassA(&wc);
    HWND p = CreateWindowExA(0, "subtest", "", WS_OVERLAPPED, 0, 0, 200, 200,
                             NULL, NULL, hi, NULL);
    HWND chk = CreateWindowExA(0, "BUTTON", "chk", WS_CHILD | BS_AUTOCHECKBOX,
                               0, 0, 120, 20, p, (HMENU)ID_CHK, hi, NULL);
    g_orig = (WNDPROC)(LONG_PTR)SetWindowLongA(chk, GWL_WNDPROC, (LONG)(LONG_PTR)Sub);
    SendMessageA(chk, BM_SETCHECK, BST_CHECKED, 0);   /* -> Sub -> CallWindowProc(orig) */
    int chk_state = (int)SendMessageA(chk, BM_GETCHECK, 0, 0);
    printf("saw=%d check=%d\n", g_saw, chk_state);
    return 0;
}
